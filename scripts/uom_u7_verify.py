#!/usr/bin/env python3
"""Relocation verifier for card UOM step U7 (gate G-UR): the PPP pipeline.

    python scripts/uom_u7_verify.py [--verbose] [--step b|c] [--tree DIR]

Step U7b cut Application::run into a mode dispatch and the PPP spine runPpp(), whose
inline blocks became named stages in concern files (PppSetup.cpp, PppEpochSteps.cpp,
PppEpochUpdate.cpp, PppEpochRecords.cpp, PppRunEnd.cpp), and moved the existing epoch
helpers beside them.  Step U7c cut Application::runBaseline the same way (setup,
per-rover, per-epoch and summary members, in ApplicationBaseline.cpp).  Step U7a deleted
dead code; those frozen lines are declared drops here.

The frozen sources are src/core/app/Application.cpp and src/core/app/ApplicationBaseline.cpp
at f823a36 (the U7 base), read with `git show`; no copy is stored.  UNITS declares, for
every function that now carries frozen code, its file, its definition and its body as an
ordered list of pieces:

  F(src, a, b, shift)   frozen lines a..b (1-based) of frozen file src ("APP" or "BASE"),
                        code lines only, with `shift` leading spaces removed (a negative
                        shift adds spaces: the body moved right);
  N(text)               a declared new code line (alias, call, return, guard), compared
                        without its comment and without leading and trailing space;
  G(src, if_line, close_line, text)
                        the frozen `if (COND) {` at if_line and its `}` at close_line
                        became the early-return guard `text` (a declared new line); both
                        frozen lines are carried;
  B(), C(text)          layout only: a blank line and a new comment line (the generator
                        of the cut writes them; this check ignores them);
  subs                  per unit: (src, frozen line, from, to), a plain text replacement on
                        that frozen line before comparison.

MEMBERS lists the frozen declarations that became members of a working value
(PppEpochWork in include/app/PppPipelineWork.hpp; the baseline values in
ApplicationBaseline.cpp), DROPS the frozen code lines no body carries, each with its reason,
DUPLICATES the frozen ranges dropped because a shared stage carries an identical copy (the
copies are checked equal).  Comments and blank lines are ignored in the code comparison, as
in scripts/uom_u3_verify.py.

Exit 0 only when every check passes:
  1. Bodies.     Each unit's body code lines equal the expected sequence, character for
                 character, indentation included.
  2. Coverage.   Every code line inside a function body of each frozen file is carried
                 exactly once: by a unit, a member, a declared drop or a duplicate.
  3. Members.    Each member declaration appears in its working value as written; its type
                 and name match the frozen declaration (an `auto` type takes the declared one).
  4. Duplicates. Each dropped duplicate range equals its carried twin, code line for line.
  5. Comments.   Every comment line of a frozen file appears in the new files at least as
                 often (whitespace-normalized), apart from the declared comment edits.
  6. Order.      The ordered list of every call (name, and the string literal that opens its
                 arguments: log format strings, logger names) of each frozen driver
                 (Application::run, Application::runBaseline) equals the new driver's, with
                 each call into a cut stage replaced by that stage's own list
                 (recursively), the member pointers of a run-end table read in table order,
                 and each frozen lambda that became a function replaced at its call sites
                 by its body's list.
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

FROZEN_REV = "f823a36"
FROZEN = {
    "APP": "src/core/app/Application.cpp",
    "BASE": "src/core/app/ApplicationBaseline.cpp",
}

APP = "src/core/app/Application.cpp"
SETUP = "src/core/app/PppSetup.cpp"
STEPS = "src/core/app/PppEpochSteps.cpp"
UPD = "src/core/app/PppEpochUpdate.cpp"
REC = "src/core/app/PppEpochRecords.cpp"
END = "src/core/app/PppRunEnd.cpp"
WORK_HDR = "include/app/PppPipelineWork.hpp"
BASEF = "src/core/app/ApplicationBaseline.cpp"
NEW_FILES = [APP, SETUP, STEPS, UPD, REC, END, WORK_HDR, BASEF]


def F(src, a, b, shift=0):
    return ("F", src, a, b, shift)


def FA(a, b, shift=0):
    return F("APP", a, b, shift)


def FB(a, b, shift=0):
    return F("BASE", a, b, shift)


def N(text):
    return ("N", text)


def G(src, if_line, close_line, text):
    return ("G", src, if_line, close_line, text)


def B():
    return ("B",)


def C(text):
    return ("C", text)


# Alias lines of the PPP epoch stages: each binds one of the loop body's former local names
# to the PppEpochWork member of that name.
PA = {
    "current_time": "const auto& current_time = w.current_time;",
    "epoch_obs": "const auto& epoch_obs = w.epoch_obs;",
    "epoch_prefix": "const auto& epoch_prefix = w.epoch_prefix;",
    "dt": "const auto& dt = w.dt;",
    "dt_w": "auto& dt = w.dt;",
    "gap_detected_w": "auto& gap_detected = w.gap_detected;",
    "long_gap_w": "auto& long_gap = w.long_gap;",
    "modeled_obs": "const auto& modeled_obs = w.modeled_obs;",
    "modeled_obs_w": "auto& modeled_obs = w.modeled_obs;",
    "drop_reasons_w": "auto& drop_reasons = w.drop_reasons;",
    "slip_result": "const auto& slip_result = w.slip_result;",
    "applied_meas": "const auto& applied_meas = w.applied_meas;",
    "have_applied": "const auto& have_applied = w.have_applied;",
    "epoch_reset_sats": "const auto& epoch_reset_sats = w.epoch_reset_sats;",
    "epoch_reset_sats_w": "auto& epoch_reset_sats = w.epoch_reset_sats;",
    "epoch_complete_reset_w": "auto& epoch_complete_reset = w.epoch_complete_reset;",
}


def pa(*names):
    return [N("    " + PA[n]) for n in names]


# ---------------------------------------------------------------------------
# Application.cpp (frozen "APP")
# ---------------------------------------------------------------------------
UNITS_APP = [
    # --- Application.cpp: kept in place.
    dict(file=APP, def_re=r"^Application::Application\(", pieces=[]),
    dict(file=APP, def_re=r"^bool Application::initLoggingAndConfig\(", pieces=[FA(84, 163)]),
    dict(file=APP, def_re=r"^bool Application::resolveData\(", pieces=[FA(177, 229)]),
    dict(file=APP, def_re=r"^void Application::refreshAtxIfEnabled_\(", pieces=[FA(233, 293)]),
    dict(file=APP, def_re=r"^bool Application::loadInputData\(", pieces=[FA(301, 337)]),
    dict(file=APP, def_re=r"^void Application::generateReport_\(", pieces=[FA(2361, 2365)]),
    dict(file=APP, def_re=r"^void Application::generateReportFor_\(", pieces=[FA(2369, 2428)]),
    # --- The existing helpers, moved unchanged to the file of their concern (`move` is the
    #     frozen range the generator of the cut copies: banner, definition and body).
    dict(file=SETUP, def_re=r"^bool Application::initializePosition\(", move=(340, 514),
         pieces=[FA(345, 513)]),
    dict(file=STEPS, def_re=r"^bool Application::collectEpochObservations\(", move=(516, 545),
         pieces=[FA(524, 544)]),
    dict(file=STEPS, def_re=r"^WlsResult Application::handleWlsSeeding\(", move=(547, 604),
         pieces=[FA(555, 603)]),
    dict(file=STEPS, def_re=r"^Application::DynamicsMode Application::classifyDynamicsMode\(",
         move=(606, 624), pieces=[FA(611, 623)]),
    dict(file=STEPS, def_re=r"^void Application::handleGap\(", move=(626, 730), pieces=[FA(630, 729)]),
    dict(file=STEPS, def_re=r"^CycleSlipEpochResult Application::detectCycleSlips\(", move=(754, 766),
         pieces=[FA(760, 765)]),
    dict(file=UPD, def_re=r"^void Application::runFilterUpdateLoop\(", move=(865, 1097),
         pieces=[FA(875, 1096)]),
    dict(file=UPD, def_re=r"^void Application::runGeometryRelinearization\(", move=(1099, 1263),
         pieces=[FA(1107, 1262)]),
    dict(file=UPD, def_re=r"^void Application::runGeometryRelinearizationNaive\(", move=(1265, 1313),
         pieces=[FA(1272, 1312)]),
    dict(file=UPD, def_re=r"^void Application::applyGimIonoUpdate\(", move=(768, 863),
         pieces=[FA(774, 862)]),
    dict(file=REC, def_re=r"^void Application::capturePriorSnapshot\(", move=(1334, 1358),
         pieces=[FA(1339, 1357)]),
    dict(file=REC, def_re=r"^void Application::capturePosteriorSnapshot\(", move=(1360, 1411),
         pieces=[FA(1363, 1410)]),
    dict(file=REC, def_re=r"^void Application::logEpochSummary\(", move=(1413, 1456),
         pieces=[FA(1418, 1455)]),
    dict(file=REC, def_re=r"^void Application::writeEpochTraces\(", move=(1458, 1505),
         pieces=[FA(1469, 1504)]),
    # --- run(): the configuration, the Eigen threads and the mode dispatch; the PPP path is
    #     runPpp().  Its first 59 lines were unindented; they moved four columns right.
    dict(file=APP, def_re=r"^int Application::run\(", section=True, pieces=[
        FA(1512, 1560, -4),
        B(),
        C("    // --- The PPP / PPP-RTK pipeline: the spine below ---"),
        N("    return runPpp();")]),
    # --- runPpp(): the spine.
    dict(file=APP, def_re=r"^int Application::runPpp\(", section=True, pieces=[
        FA(1562, 1570, -4),
        B(),
        FA(1572, 1572),
        B(),
        C("    // The components and their wiring, in this order (PppSetup.cpp)."),
        N("    buildPppComponents_();          // PppModel, MeasurementModel, KalmanFilter, WLS, CycleSlipManager"),
        N("    buildGlobalIonosphereMap_();    // the GIM, for ionosphere weighting or the higher-order term"),
        N("    buildIonoVariabilityMonitor_(); // estimation.adaptive_iono_process_noise"),
        N("    buildNeqAccumulator_();         // processing.enable_neq_export"),
        N("    wireCycleSlipManager_();        // biases, MW gap tolerance, sample interval, KF tiebreak"),
        N("    enablePppTraceStreams_();       // MW, TurboEdit, wind-up, census and iono-monitor traces"),
        N("    buildAidingSources_();          // the IMU and the other aiding sources"),
        N("    initializePppState_();          // PppModel::initializeState; the SIF a-priori re-applied"),
        N("    buildAmbiguityResolver_();      // MLAMBDA, when ambiguity_resolution.mode > 0"),
        B(),
        FA(1765, 1765),
        N("    buildTraceLogger_(is_ppp_rtk);  // the CSV trace streams"),
        B(),
        FA(1774, 1779),
        B(),
        C("    // One epoch: the stages of PppEpochSteps.cpp, PppEpochUpdate.cpp and"),
        C("    // PppEpochRecords.cpp, in this order, over the epoch's PppEpochWork."),
        FA(1781, 1781),
        N("        PppEpochWork w;"),
        FA(1782, 1782),
        FA(1785, 1786),
        N("        if (skipDecimatedEpoch_(w)) continue;   // 4a2 processing.gnss_rate_s decimation"),
        N("        if (reachedMaxDuration_(w)) break;      // 4a3 processing.max_duration_s cutoff"),
        N("        stepEpochClock_(w);                     // 4b  epoch stamp, time step, data gap"),
        B(),
        FA(1865, 1865),
        N("        if (!syncPppState_(w)) return 1;"),
        N("        if (!computeEpochModels_(w)) continue;  // 4d  modeled observations, drop reasons"),
        B(),
        FA(1893, 1894),
        B(),
        FA(1896, 1903),
        B(),
        N("        refreshSubstitutedAtxQ_(w);             // 4e2 ATX-substituted ambiguity Q overrides"),
        N("        refreshAdaptiveIonoQ_(w);               // 4e3 adaptive ionospheric Q overrides"),
        N("        predictEpoch_(w);                       // 4f  Kalman prediction; first_epoch_ cleared"),
        B(),
        FA(1992, 1995),
        N("        if (!syncPppState_(w)) return 1;        //     ppp_state_ after the predict"),
        B(),
        FA(2004, 2006),
        N("        foldSlipRepairsIntoPrior_(w);           //     repairs into the smoother a-priori"),
        B(),
        FA(2031, 2031),
        FA(2036, 2037),
        B(),
        FA(2039, 2040),
        N("        mergeEpochResets_(w);                   //     KF, relinearization and slip resets"),
        B(),
        FA(2056, 2061),
        B(),
        FA(2063, 2066),
        B(),
        N("        accumulateNormalEquations_(w);          //     processing.enable_neq_export"),
        N("        recordBridgeSnapshot_(w);               //     the soft-LLI bridge's last-good snapshot"),
        N("        resolveEpochAmbiguities_(w);            // 4i2 integer ambiguity resolution (MLAMBDA)"),
        B(),
        FA(2193, 2197),
        B(),
        C("    // --- Phase 5: the run end, the reports of kPppRunEndReports in order (PppRunEnd.cpp) ---"),
        N("    reportPppRunEnd_(PppRunEndView{ wall_start, is_ppp_rtk });"),
        B(),
        FA(2354, 2357)],
        subs=[("APP", 1785, "epoch_obs, current_time", "w.epoch_obs, w.current_time"),
              ("APP", 1894, "const auto wls_result = handleWlsSeeding(current_time, epoch_prefix, "
                            "modeled_obs, gap_detected);",
               "w.wls_result = handleWlsSeeding(w.current_time, w.epoch_prefix, w.modeled_obs, "
               "w.gap_detected);"),
              ("APP", 1901, "(gap_detected)", "(w.gap_detected)"),
              ("APP", 1902, "handleGap(dt, long_gap, epoch_prefix, wls_result)",
               "handleGap(w.dt, w.long_gap, w.epoch_prefix, w.wls_result)"),
              ("APP", 1995, "(current_time, dt)", "(w.current_time, w.dt)"),
              ("APP", 2005, "const CycleSlipEpochResult slip_result =", "w.slip_result ="),
              ("APP", 2006, "(epoch_prefix, modeled_obs)", "(w.epoch_prefix, w.modeled_obs)"),
              ("APP", 2036, "runFilterUpdateLoop(epoch_prefix, modeled_obs, applied_meas, have_applied,",
               "runFilterUpdateLoop(w.epoch_prefix, w.modeled_obs, w.applied_meas, w.have_applied,"),
              ("APP", 2037, "epoch_reset_sats, epoch_complete_reset", "w.epoch_reset_sats, w.epoch_complete_reset"),
              ("APP", 2040, "(epoch_prefix, epoch_obs, applied_meas, have_applied)",
               "(w.epoch_prefix, w.epoch_obs, w.applied_meas, w.have_applied)"),
              ("APP", 2061, "(current_time, modeled_obs)", "(w.current_time, w.modeled_obs)"),
              ("APP", 2066, "(epoch_reset_sats, epoch_complete_reset, have_applied)",
               "(w.epoch_reset_sats, w.epoch_complete_reset, w.have_applied)"),
              ("APP", 2194, "(epoch_prefix)", "(w.epoch_prefix)"),
              ("APP", 2195, "writeEpochTraces(current_time, epoch_obs, modeled_obs, drop_reasons,",
               "writeEpochTraces(w.current_time, w.epoch_obs, w.modeled_obs, w.drop_reasons,"),
              ("APP", 2196, "applied_meas, have_applied, epoch_reset_sats",
               "w.applied_meas, w.have_applied, w.epoch_reset_sats")]),
    # --- PppSetup.cpp: the setup stages, one block of the former run() each.
    dict(file=SETUP, def_re=r"^void Application::buildPppComponents_\(", section=True,
         pieces=[FA(1574, 1578)]),
    dict(file=SETUP, def_re=r"^void Application::buildGlobalIonosphereMap_\(", section=True,
         pieces=[FA(1580, 1613)]),
    dict(file=SETUP, def_re=r"^void Application::buildIonoVariabilityMonitor_\(", section=True,
         pieces=[FA(1615, 1621)]),
    dict(file=SETUP, def_re=r"^void Application::buildNeqAccumulator_\(", section=True,
         pieces=[FA(1623, 1633)]),
    dict(file=SETUP, def_re=r"^void Application::wireCycleSlipManager_\(", section=True,
         pieces=[FA(1635, 1672)]),
    dict(file=SETUP, def_re=r"^void Application::enablePppTraceStreams_\(", section=True,
         pieces=[FA(1674, 1693)]),
    dict(file=SETUP, def_re=r"^void Application::buildAidingSources_\(", section=True,
         pieces=[FA(1695, 1712)]),
    dict(file=SETUP, def_re=r"^void Application::initializePppState_\(", section=True,
         pieces=[FA(1714, 1729)]),
    dict(file=SETUP, def_re=r"^void Application::buildAmbiguityResolver_\(", section=True,
         pieces=[FA(1731, 1763)]),
    dict(file=SETUP, def_re=r"^void Application::buildTraceLogger_\(", section=True,
         pieces=[FA(1766, 1772)]),
    # --- PppEpochSteps.cpp: the epoch up to the measurement update.
    dict(file=STEPS, def_re=r"^bool Application::skipDecimatedEpoch_\(", section=True, pieces=[
        *pa("current_time"), B(), FA(1788, 1799, 4), N("    return false;")],
        subs=[("APP", 1797, "continue;", "return true;")]),
    dict(file=STEPS, def_re=r"^bool Application::reachedMaxDuration_\(", section=True, pieces=[
        *pa("current_time"), B(), FA(1801, 1823, 4), N("    return false;")],
        subs=[("APP", 1820, "break;", "return true;")]),
    dict(file=STEPS, def_re=r"^void Application::stepEpochClock_\(", section=True, pieces=[
        *pa("current_time", "epoch_prefix", "dt_w", "gap_detected_w", "long_gap_w"), B(),
        FA(1825, 1826, 4), B(), FA(1834, 1838, 4), FA(1842, 1863, 4)]),
    dict(file=STEPS, def_re=r"^bool Application::syncPppState_\(", section=True, pieces=[
        *pa("epoch_prefix"), B(), FA(1866, 1871, 4), N("    return true;")],
        subs=[("APP", 1870, "return 1;", "return false;")]),
    dict(file=STEPS, def_re=r"^bool Application::computeEpochModels_\(", section=True, pieces=[
        *pa("epoch_obs", "epoch_prefix", "modeled_obs_w", "drop_reasons_w"), B(),
        FA(1873, 1877, 4), FA(1880, 1891, 4), N("    return true;")],
        subs=[("APP", 1882, "continue;", "return false;")]),
    dict(file=STEPS, def_re=r"^void Application::refreshSubstitutedAtxQ_\(", section=True, pieces=[
        *pa("epoch_prefix"), B(), FA(1905, 1914, 4), FA(1916, 1957, 8)]),
    dict(file=STEPS, def_re=r"^void Application::refreshAdaptiveIonoQ_\(", section=True, pieces=[
        *pa("modeled_obs"), B(), FA(1960, 1970, 4)]),
    dict(file=STEPS, def_re=r"^void Application::predictEpoch_\(", section=True, pieces=[
        *pa("current_time", "dt"), B(), FA(1972, 1990, 4)]),
    # --- PppEpochUpdate.cpp: the merged resets and ambiguity resolution.
    dict(file=UPD, def_re=r"^void Application::mergeEpochResets_\(", section=True, pieces=[
        *pa("slip_result", "epoch_reset_sats_w", "epoch_complete_reset_w"), B(), FA(2042, 2054, 4)]),
    dict(file=UPD, def_re=r"^void Application::resolveEpochAmbiguities_\(", section=True, pieces=[
        *pa("epoch_prefix", "modeled_obs", "have_applied"), B(),
        FA(2097, 2097, 4),
        G("APP", 2098, 2188, "    if (!ambiguity_resolver_ || !have_applied) return;"),
        FA(2099, 2100, 8),
        B(),
        C("    // The AR-side ATX gates withhold integer fixing on the WL entries whose"),
        C("    // signals' antenna corrections are missing or frequency-substituted."),
        N("    dropAtxGatedWlEntries_(epoch_prefix, wl_ambs, wl_fracs);"),
        B(),
        FA(2167, 2187, 8)]),
    dict(file=UPD, def_re=r"^void Application::dropAtxGatedWlEntries_\(", section=True, pieces=[
        FA(2102, 2165, 8)]),
    # --- PppEpochRecords.cpp: the smoother fold, the NEQ, the soft-LLI snapshot.
    dict(file=REC, def_re=r"^void Application::foldSlipRepairsIntoPrior_\(", section=True, pieces=[
        *pa("slip_result"), B(), FA(2008, 2029, 4)]),
    dict(file=REC, def_re=r"^void Application::accumulateNormalEquations_\(", section=True, pieces=[
        *pa("epoch_prefix", "applied_meas", "have_applied", "epoch_reset_sats"), B(),
        FA(2068, 2086, 4)]),
    dict(file=REC, def_re=r"^void Application::recordBridgeSnapshot_\(", section=True, pieces=[
        *pa("current_time"), B(), FA(2088, 2095, 4)]),
    # --- PppRunEnd.cpp: the run end as a table of reports.
    dict(file=END, def_re=r"^void Application::reportPppRunEnd_\(", section=True, table=True, pieces=None),
    dict(file=END, def_re=r"^void Application::reportWindUpCensus_\(", section=True,
         pieces=[FA(2199, 2235)]),
    dict(file=END, def_re=r"^void Application::runSmootherPass_\(", section=True, pieces=[
        FA(2237, 2244), N("            writeSmootherTrace_(smoother);"), FA(2289, 2290)]),
    dict(file=END, def_re=r"^void Application::writeSmootherTrace_\(", section=True,
         pieces=[FA(2245, 2288, 8)]),
    dict(file=END, def_re=r"^void Application::exportNormalEquations_\(", section=True,
         pieces=[FA(2292, 2337)]),
    dict(file=END, def_re=r"^void Application::reportProcessingTime_\(", section=True, pieces=[
        N("    const auto& wall_start = v.wall_start;"), N("    const bool is_ppp_rtk = v.is_ppp_rtk;"),
        B(), FA(2339, 2342)]),
    dict(file=END, def_re=r"^void Application::reportReinflations_\(", section=True, pieces=[
        N("    const bool is_ppp_rtk = v.is_ppp_rtk;"), B(), FA(2343, 2349)]),
    dict(file=END, def_re=r"^void Application::reportKalmanFilterRunEnd_\(", section=True,
         pieces=[FA(2351, 2351)]),
    dict(file=END, def_re=r"^void Application::reportCycleSlipRunEnd_\(", section=True,
         pieces=[FA(2352, 2352)]),
]

# The run-end table's walk: its rows (member pointers, in order) are the report calls.
TABLE_WALK = [
    "    using Report = void (Application::*)(const PppRunEndView&);",
    "    struct Row {",
    "        const char* name;      // what the report prints",
    "        const char* armed_by;  // the knob that arms it",
    "        Report      report;    // the report",
    "    };",
    "    static const Row kPppRunEndReports[] = {",
    '        { "wind-up census",             "estimation.trace_windup_census", &Application::reportWindUpCensus_ },',
    '        { "RTS smoother and its trace", "processing.run_smoother",        &Application::runSmootherPass_ },',
    '        { "normal-equation export",     "processing.enable_neq_export",   &Application::exportNormalEquations_ },',
    '        { "processing time",            "always",                         &Application::reportProcessingTime_ },',
    '        { "state re-inflations",        "always",                         &Application::reportReinflations_ },',
    '        { "Kalman filter timings",      "always",                         &Application::reportKalmanFilterRunEnd_ },',
    '        { "cycle-slip counters",        "always",                         &Application::reportCycleSlipRunEnd_ },',
    "    };",
    "",
    "    for (const Row& row : kPppRunEndReports) (this->*row.report)(v);",
]
for u in UNITS_APP:
    if u.get("table"):
        u["pieces"] = [N(t) if t else B() for t in TABLE_WALK]

MEMBERS_APP = {
    1783: ("PppEpochWork", "GnssTime current_time{};"),
    1784: ("PppEpochWork", "std::vector<RinexObservation> epoch_obs;"),
    1832: ("PppEpochWork", "const std::string epoch_prefix;"),
    1839: ("PppEpochWork", "double dt{ 0.0 };"),
    1840: ("PppEpochWork", "bool gap_detected{ false };"),
    1841: ("PppEpochWork", "bool long_gap{ false };"),
    1878: ("PppEpochWork", "std::vector<PppObservation> modeled_obs;"),
    1879: ("PppEpochWork", "std::map<std::string, DropReason> drop_reasons;"),
    1894: ("PppEpochWork", "WlsResult wls_result;"),
    2005: ("PppEpochWork", "CycleSlipEpochResult slip_result;"),
    2032: ("PppEpochWork", "EpochMeasurement applied_meas{};"),
    2033: ("PppEpochWork", "bool have_applied{ false };"),
    2034: ("PppEpochWork", "std::set<std::string> epoch_reset_sats;"),
    2035: ("PppEpochWork", "bool epoch_complete_reset{ false };"),
}
# Lines that are both a member (the declaration) and carried (the initializer became an
# assignment in the spine); MEMBERS documents the type, the unit carries the line.
MEMBER_ALSO_CARRIED_APP = {1894, 2005}

DROPS_APP = {
    **{i: "Application::gpsToUtcTime deleted in step U7a (no caller; RealTimeEngine keeps its own)"
       for i in range(71, 77)},
    **{i: "the unused anonymous-namespace satIdFromAmbiguityKey deleted in step U7a"
       for i in range(739, 743)},
    **{i: "the unused anonymous-namespace signalFromAmbiguityKey deleted in step U7a"
       for i in range(747, 751)},
    **{i: "Application::updatePositionSigma deleted in step U7a (its member was never read)"
       for i in range(1320, 1332)},
    2191: "the call of updatePositionSigma, deleted in step U7a",
    1915: "the bare block of the ATX Q overrides became the body of refreshSubstitutedAtxQ_",
    1958: "the closing brace of that block",
}

# Frozen lines deleted with their calls (step U7a): the event list of check 6 leaves them out.
DELETED_APP = {2191}

# Frozen ranges dropped because a shared stage carries an identical copy: (src, dropped a,
# dropped b) -> (src, carried a, carried b, the stage that carries it).
DUPLICATES_APP = {
    ("APP", 1997, 2002): ("APP", 1866, 1871, "syncPppState_ (both sync sites of the epoch)"),
}

# Frozen lambdas that became functions: (src, name): (definition first line, last line,
# first line of the body, new function name).
LAMBDAS_APP = {}

# Comment lines of the frozen files deliberately edited or removed (normalized text: why).
COMMENT_EDITS = {
    # Step U7a.
    "GPS -> UTC helper (used by trace logging)": "the dead gpsToUtcTime was deleted (U7a)",
    "Epoch helper: MW + IF cycle slip detection": "the banner of the deleted dead helpers (U7a)",
    "Extract satellite ID from an ambiguity key (e.g. \"G01_L1C\" -> \"G01\").": "deleted helper (U7a)",
    "Extract signal from an ambiguity key (e.g. \"G01_L1C\" -> \"L1C\").": "deleted helper (U7a)",
    "Epoch helper: Update stored 3D position sigma": "the deleted updatePositionSigma (U7a)",
    "4j. Store posterior position sigma for IF slip gating on the next epoch.": "its deleted call (U7a)",
    "applied (gpsToUtcTime() is not used for traces), and consumers apply":
        "names the deleted function; now `applied (the traces are not converted to UTC)` (U7a)",
    "- updatePositionSigma()       – posterior 3D position uncertainty extraction.":
        "the deleted helper's line of the file banner (U7a)",
    "anonymous namespace": "the closing comment of the deleted anonymous namespace (U7a)",
    # Step U7b: the file banner's helper list became the file map of the cut.
    "Implements the Application class which orchestrates initialization of": "file banner rewritten",
    "configuration, logging and processing components (product parser, RINEX": "file banner rewritten",
    "parser, PPP model, measurement model, Kalman filter, WLS estimator, etc.)": "file banner rewritten",
    "and runs the epoch-by-epoch processing loop.": "file banner rewritten",
    "The pipeline is decomposed into focused private helpers called from run():": "file banner rewritten",
    "- initLoggingAndConfig()      – logging setup and JSON configuration load.": "file banner rewritten",
    "- loadInputData()             – RINEX parsing and product cache population.": "file banner rewritten",
    "- initializePosition()        – filter state seeding from RINEX header.": "file banner rewritten",
    "- collectEpochObservations()  – per-epoch observation grouping and filtering.": "file banner rewritten",
    "- handleWlsSeeding()          – WLS estimation and KF clock seeding.": "file banner rewritten",
    "- detectCycleSlips()          – MW and IF phase slip detection / resets.": "file banner rewritten",
    "- runFilterUpdateLoop()       – iterative KF measurement update loop.": "file banner rewritten",
    "- runGeometryRelinearization()– optional second-pass geometry update.": "file banner rewritten",
    "- logEpochSummary()           – per-epoch state logging.": "file banner rewritten",
    "- writeEpochTraces()          – CSV trace output for offline analysis.": "file banner rewritten",
    "@brief Top-level application driver for GNSS processing.": "file banner rewritten",
}

# ---------------------------------------------------------------------------
# ApplicationBaseline.cpp (frozen "BASE")
# ---------------------------------------------------------------------------
# Step U7b leaves runBaseline as it is.
UNITS_BASE_B = [
    dict(file=BASEF, def_re=r"^int Application::runBaseline\(", section=True, pieces=[FB(37, 1077)]),
]
MEMBERS_BASE_B = {}
DROPS_BASE_B = {}
DUPLICATES_BASE_B = {}
LAMBDAS_BASE_B = {}
ALLOWED_EXTRA_BASE_B = []  # (unit, event) pairs the cut adds

# Step U7c: runBaseline as a spine over setup, per-rover, per-epoch and summary members, all in
# ApplicationBaseline.cpp; the baseline working values are defined there too.
#
# Alias lines of the baseline stages: each binds one of runBaseline's former local names to the
# member of that name of BaselineRunWork (run), BaselineRoverWork (rover) or BaselineEpochWork (ep).
BA = {
    "log": "const auto& log = run.log;",
    "bc": "const auto& bc = run.bc;",
    "sync": "const auto& sync = run.sync;",
    "synced": "const auto& synced = run.synced;",
    "ref_pos": "const auto& ref_pos = run.ref_pos;",
    "mw_ref": "auto& mw_ref = run.mw_ref;",
    "active_set": "const auto& active_set = run.active_set;",
    "multi_rover": "const bool multi_rover = run.multi_rover;",
    "rovers_with_data_w": "auto& rovers_with_data = run.rovers_with_data;",
    "total_rover_updates": "const auto& total_rover_updates = run.total_rover_updates;",
    "total_rover_dd_obs": "const auto& total_rover_dd_obs = run.total_rover_dd_obs;",
    "rover_name": "const auto& rover_name = rover.rover_name;",
    "rov_apriori": "const auto& rov_apriori = rover.rov_apriori;",
    "rover_trace_dir": "const auto& rover_trace_dir = rover.rover_trace_dir;",
    "mw_rov": "auto& mw_rov = rover.mw_rov;",
    "trace_ofs_w": "auto& trace_ofs = rover.trace_ofs;",
    "trace_path": "const auto& trace_path = rover.trace_path;",
    "trace_path_w": "auto& trace_path = rover.trace_path;",
    "n_with_dd": "const auto& n_with_dd = rover.n_with_dd;",
    "n_with_dd_w": "auto& n_with_dd = rover.n_with_dd;",
    "total_dd_obs": "const auto& total_dd_obs = rover.total_dd_obs;",
    "total_dd_obs_w": "auto& total_dd_obs = rover.total_dd_obs;",
    "total_code_dd": "const auto& total_code_dd = rover.total_code_dd;",
    "total_code_dd_w": "auto& total_code_dd = rover.total_code_dd;",
    "total_phase_dd": "const auto& total_phase_dd = rover.total_phase_dd;",
    "total_phase_dd_w": "auto& total_phase_dd = rover.total_phase_dd;",
    "total_doppler_dd": "const auto& total_doppler_dd = rover.total_doppler_dd;",
    "total_doppler_dd_w": "auto& total_doppler_dd = rover.total_doppler_dd;",
    "n_geom_ok": "const auto& n_geom_ok = rover.n_geom_ok;",
    "n_geom_ok_w": "auto& n_geom_ok = rover.n_geom_ok;",
    "n_updates": "const auto& n_updates = rover.n_updates;",
    "n_updates_w": "auto& n_updates = rover.n_updates;",
    "pivot_counts": "const auto& pivot_counts = rover.pivot_counts;",
    "pivot_counts_w": "auto& pivot_counts = rover.pivot_counts;",
    "constellation_dd": "const auto& constellation_dd = rover.constellation_dd;",
    "constellation_dd_w": "auto& constellation_dd = rover.constellation_dd;",
    "res_stats_t": "using ResStats = BaselineRoverWork::ResStats;",
    "res_code": "const auto& res_code = rover.res_code;",
    "res_code_w": "auto& res_code = rover.res_code;",
    "res_phase": "const auto& res_phase = rover.res_phase;",
    "res_phase_w": "auto& res_phase = rover.res_phase;",
    "res_doppler": "const auto& res_doppler = rover.res_doppler;",
    "res_doppler_w": "auto& res_doppler = rover.res_doppler;",
    "prev_tow": "const auto& prev_tow = rover.prev_tow;",
    "prev_tow_w": "auto& prev_tow = rover.prev_tow;",
    "prev_week": "const auto& prev_week = rover.prev_week;",
    "prev_week_w": "auto& prev_week = rover.prev_week;",
    "max_dur_anchor_s_w": "auto& max_dur_anchor_s = rover.max_dur_anchor_s;",
    "max_dur_anchored_w": "auto& max_dur_anchored = rover.max_dur_anchored;",
    "n_slips_ref": "const auto& n_slips_ref = rover.n_slips_ref;",
    "n_slips_ref_w": "auto& n_slips_ref = rover.n_slips_ref;",
    "n_slips_rov": "const auto& n_slips_rov = rover.n_slips_rov;",
    "n_slips_rov_w": "auto& n_slips_rov = rover.n_slips_rov;",
    "n_amb_resets": "const auto& n_amb_resets = rover.n_amb_resets;",
    "n_amb_resets_w": "auto& n_amb_resets = rover.n_amb_resets;",
    "se": "const auto& se = ep.se;",
    "rov_it": "const auto& rov_it = ep.rov_it;",
    "rov_it_w": "auto& rov_it = ep.rov_it;",
    "ref_it": "const auto& ref_it = ep.ref_it;",
    "ref_it_w": "auto& ref_it = ep.ref_it;",
    "both_present": "const auto& both_present = ep.both_present;",
    "both_present_w": "auto& both_present = ep.both_present;",
    "dt": "const auto& dt = ep.dt;",
    "dt_w": "auto& dt = ep.dt;",
    "rov_modeled": "const auto& rov_modeled = ep.rov_modeled;",
    "rov_modeled_w": "auto& rov_modeled = ep.rov_modeled;",
    "ref_modeled": "const auto& ref_modeled = ep.ref_modeled;",
    "ref_modeled_w": "auto& ref_modeled = ep.ref_modeled;",
    "em": "const auto& em = ep.em;",
    "em_w": "auto& em = ep.em;",
    "carry_excluded": "const auto& carry_excluded = ep.carry_excluded;",
    "carry_excluded_w": "auto& carry_excluded = ep.carry_excluded;",
    "built_count": "const auto& built_count = ep.built_count;",
    "built_count_w": "auto& built_count = ep.built_count;",
    "applied": "const auto& applied = ep.applied;",
    "applied_w": "auto& applied = ep.applied;",
    "committed": "const auto& committed = ep.committed;",
    "committed_w": "auto& committed = ep.committed;",
    "epoch_failed_w": "auto& epoch_failed = ep.epoch_failed;",
}


def ba(*names):
    return [N("    " + BA[n]) for n in names]


UNITS_BASE_C = [
    # --- The two lambdas that became functions (anonymous namespace).
    dict(file=BASEF, def_re=r"^std::vector<PppObservation> toMinimalPppObs\(", section=True,
         pieces=[FB(241, 275, 4)]),
    dict(file=BASEF, def_re=r"^std::string constellationOfSatPairId\(", section=True,
         pieces=[FB(731, 741, 12)]),
    # --- runBaseline: the spine.
    dict(file=BASEF, def_re=r"^int Application::runBaseline\(", section=True, pieces=[
        FB(37, 43),
        N("    if (!resolveBaselineData_(log, bc_initial)) return 1;   // Phase 0: the PathResolver pass"),
        B(),
        FB(63, 64),
        N("    if (!checkBaselineInputs_(log, bc)) return 1;           // the roster; a RINEX path per station"),
        B(),
        FB(107, 118),
        N("    if (!synchronizeBaselineStations_(log, sync)) return 1; // load the stations, align their epochs"),
        N("    const auto& synced = sync.getSyncedEpochs();"),
        N("    if (!loadBaselineProducts_(log)) return 1;              // Phase 2: the product cache (SP3 orbits)"),
        B(),
        FB(158, 175),
        N("    buildBaselineReference_(ref_parser, ref_pos, synced);   // its StateManager, PppModel and GIM"),
        B(),
        FB(207, 218),
        B(),
        FB(278, 290),
        N("    BaselineRunWork run{ log, bc, sync, synced, ref_pos, mw_ref, active_set, (bc.rovers.size() > 1) };"),
        B(),
        FB(296, 296),
        N("        if (!processBaselineRover_(run, rover_idx)) return 1;  // the early-abort diagnostic fired"),
        FB(1036, 1036),
        B(),
        FB(1038, 1047),
        B(),
        N("    reportBaselineAggregate_(run);"),
        B(),
        FB(1065, 1077)],
        subs=[("BASE", 1042, "if (rovers_with_data == 0) {", "if (run.rovers_with_data == 0) {"),
              ("BASE", 1069, "if (multi_rover) {", "if (run.multi_rover) {")]),
    # --- Setup stages.
    dict(file=BASEF, def_re=r"^bool Application::resolveBaselineData_\(", section=True, pieces=[
        FB(44, 61), N("    return true;")],
        subs=[("BASE", 60, "return 1;", "return false;")]),
    dict(file=BASEF, def_re=r"^bool Application::checkBaselineInputs_\(", section=True, pieces=[
        FB(65, 105), N("    return true;")],
        subs=[("BASE", 90, "return 1;", "return false;"), ("BASE", 98, "return 1;", "return false;")]),
    dict(file=BASEF, def_re=r"^bool Application::synchronizeBaselineStations_\(", section=True, pieces=[
        FB(119, 135), N("    return true;")],
        subs=[("BASE", 121, "return 1;", "return false;"), ("BASE", 125, "return 1;", "return false;"),
              ("BASE", 132, "return 1;", "return false;")]),
    dict(file=BASEF, def_re=r"^bool Application::loadBaselineProducts_\(", section=True, pieces=[
        FB(137, 156), N("    return true;")],
        subs=[("BASE", 148, "return 1;", "return false;"), ("BASE", 155, "return 1;", "return false;")]),
    dict(file=BASEF, def_re=r"^void Application::buildBaselineReference_\(", section=True, pieces=[
        FB(177, 205)]),
    # --- One rover.
    dict(file=BASEF, def_re=r"^bool Application::processBaselineRover_\(", section=True, pieces=[
        *ba("log", "bc", "sync", "synced", "ref_pos", "mw_ref"), B(),
        FB(297, 298, 4), B(),
        FB(300, 301, 4), B(),
        FB(303, 315, 4), B(),
        FB(317, 321, 4), B(),
        N("    if (!roverOverlapsReference_(run, rover_name)) return true;"),
        N("    const Eigen::Vector3d rov_apriori = baselineRoverApriori_(run, rover_cfg, rov_parser);"),
        N("    buildBaselineRoverFilter_(rov_parser, rov_apriori, synced);"),
        N("    buildBaselineRoverAr_(run, rover_idx, rover_name);"),
        N("    const std::filesystem::path rover_trace_dir = openBaselineRoverTraces_(run, rover_name);"),
        B(),
        FB(462, 463, 4), B(),
        C("    // The rover's working value: its counters, its trace and its previous epoch."),
        N("    BaselineRoverWork rover{ rover_name, rov_apriori, rover_trace_dir, mw_rov };"),
        N("    openBaselineTraceCsv_(run, rover);"),
        B(),
        FB(524, 524, 4),
        N("        const BaselineEpochAction action = processBaselineEpoch_(run, rover, se);"),
        N("        if (action == BaselineEpochAction::StopRover) break;"),
        N("        if (action == BaselineEpochAction::AbortRun) return false;"),
        FB(960, 960, 4), B(),
        N("    closeBaselineTraceCsv_(run, rover);"),
        N("    reportBaselineDdBuild_(run, rover);"),
        N("    reportBaselineSolution_(run, rover);"),
        B(),
        FB(1031, 1034, 4),
        N("    return true;")],
        subs=[("BASE", 320, "continue;", "return true;"),
              ("BASE", 1033, "total_rover_updates += n_updates;", "run.total_rover_updates += rover.n_updates;"),
              ("BASE", 1034, "total_rover_dd_obs  += total_dd_obs;", "run.total_rover_dd_obs  += rover.total_dd_obs;")]),
    dict(file=BASEF, def_re=r"^bool Application::roverOverlapsReference_\(", section=True, pieces=[
        *ba("log", "bc", "synced", "rovers_with_data_w"), B(),
        FB(323, 330, 4), FB(332, 348, 8), N("    return true;")],
        subs=[("BASE", 344, "continue;", "return false;")]),
    dict(file=BASEF, def_re=r"^Eigen::Vector3d Application::baselineRoverApriori_\(", section=True, pieces=[
        *ba("log", "bc", "ref_pos"), N("    const std::string& rover_name = rover_cfg.name;"), B(),
        FB(351, 375, 4), N("    return rov_apriori;")]),
    dict(file=BASEF, def_re=r"^void Application::buildBaselineRoverFilter_\(", section=True, pieces=[
        FB(377, 399, 4)]),
    dict(file=BASEF, def_re=r"^void Application::buildBaselineRoverAr_\(", section=True, pieces=[
        *ba("log"), B(), FB(401, 437, 4)]),
    dict(file=BASEF, def_re=r"^std::filesystem::path Application::openBaselineRoverTraces_\(", section=True, pieces=[
        *ba("log", "multi_rover"), B(), FB(439, 460, 4), N("    return rover_trace_dir;")]),
    dict(file=BASEF, def_re=r"^void Application::openBaselineTraceCsv_\(", section=True, pieces=[
        *ba("log", "rover_name", "rover_trace_dir", "trace_ofs_w", "trace_path_w"), B(),
        FB(480, 480, 4), FB(482, 482, 4), FB(484, 495, 4)]),
    dict(file=BASEF, def_re=r"^std::size_t Application::eraseBaselineAmbiguitiesForSat_\(", section=True,
         pieces=[FB(469, 477, 8)]),
    # --- One epoch of one rover.
    dict(file=BASEF, def_re=r"^BaselineEpochAction Application::processBaselineEpoch_\(", section=True, pieces=[
        FB(525, 528, 8),
        N("    if (reachedBaselineMaxDuration_(run, rover, se)) return BaselineEpochAction::StopRover;"),
        B(),
        C("    // The epoch's working value: station presence, the time step, the models, the update."),
        N("    BaselineEpochWork ep{ se };"),
        N("    locateBaselineStations_(run, rover, ep);   // station presence and the time step"),
        N("    resetAtBaselineDataGap_(run, rover, ep);   // a hard data gap: DD ambiguities, MW arcs, pivots"),
        N("    screenBaselineMwSlips_(run, rover, ep);    // the MW pre-filter on both stations"),
        B(),
        FB(667, 672, 8),
        B(),
        N("    if (!modelBaselineEpoch_(run, rover, ep)) return BaselineEpochAction::Next;"),
        N("    runBaselineUpdateLoop_(run, rover, ep);"),
        FB(853, 853, 8),
        B(),
        N("    recordCommittedBaselineEpoch_(run, rover, ep);   // residual statistics, AR, traces"),
        N("    if (abortBaselineWithoutUpdates_(run, rover, ep)) return BaselineEpochAction::AbortRun;"),
        N("    writeBaselineTraceRow_(rover, ep);"),
        N("    return BaselineEpochAction::Next;")],
        subs=[("BASE", 672, "if (!both_present) continue;", "if (!ep.both_present) return BaselineEpochAction::Next;"),
              ("BASE", 853, "if (epoch_failed) continue;", "if (ep.epoch_failed) return BaselineEpochAction::Next;")]),
    dict(file=BASEF, def_re=r"^bool Application::reachedBaselineMaxDuration_\(", section=True, pieces=[
        *ba("log", "max_dur_anchor_s_w", "max_dur_anchored_w"), B(),
        FB(530, 545, 8), N("    return false;")],
        subs=[("BASE", 543, "break;", "return true;")]),
    dict(file=BASEF, def_re=r"^void Application::locateBaselineStations_\(", section=True, pieces=[
        *ba("bc", "rover_name", "se", "prev_tow", "prev_week", "rov_it_w", "ref_it_w", "both_present_w",
            "dt_w"), B(),
        FB(547, 558, 8)],
        subs=[("BASE", 552, "auto rov_it =", "rov_it ="), ("BASE", 553, "auto ref_it =", "ref_it ="),
              ("BASE", 554, "const bool both_present =", "both_present ="),
              ("BASE", 557, "const double dt =", "dt =")]),
    dict(file=BASEF, def_re=r"^void Application::resetAtBaselineDataGap_\(", section=True, pieces=[
        *ba("log", "mw_ref", "mw_rov", "n_amb_resets_w", "prev_tow", "se", "both_present", "dt"), B(),
        FB(560, 613, 8)]),
    dict(file=BASEF, def_re=r"^void Application::screenBaselineMwSlips_\(", section=True, pieces=[
        *ba("log", "bc", "ref_pos", "mw_ref", "active_set", "rover_name", "mw_rov", "n_slips_ref_w",
            "n_slips_rov_w", "n_amb_resets_w", "se"), B(),
        FB(615, 620, 8), FB(622, 664, 12)],
        subs=[("BASE", 648, "eraseAmbiguitiesForSat(sat)", "eraseBaselineAmbiguitiesForSat_(sat)")]),
    dict(file=BASEF, def_re=r"^bool Application::modelBaselineEpoch_\(", section=True, pieces=[
        *ba("log", "prev_week_w", "prev_tow_w", "n_with_dd_w", "n_geom_ok_w", "se", "rov_it", "ref_it", "dt",
            "rov_modeled_w", "ref_modeled_w"), B(),
        FB(674, 694, 8), FB(696, 704, 8), N("    return true;")],
        subs=[("BASE", 699, "continue;", "return false;"), ("BASE", 701, "continue;", "return false;")]),
    dict(file=BASEF, def_re=r"^void Application::runBaselineUpdateLoop_\(", section=True, pieces=[
        *ba("log", "n_updates_w", "se", "rov_modeled", "ref_modeled", "em_w", "carry_excluded_w",
            "built_count_w", "applied_w", "committed_w", "epoch_failed_w"), B(),
        FB(706, 727, 8), B(),
        FB(751, 751, 8),
        N("        if (!buildBaselineDdPass_(run, ep, pass)) {"),
        N("            epoch_failed = true;"),
        N("            break;"),
        N("        }"),
        FB(772, 811, 8),
        N("        drainBaselinePivotHandovers_(log);"),
        B(),
        FB(833, 852, 8)]),
    dict(file=BASEF, def_re=r"^bool Application::buildBaselineDdPass_\(", section=True, pieces=[
        *ba("log", "rov_modeled", "ref_modeled", "carry_excluded", "em_w"), B(),
        FB(752, 761, 16), N("        return false;"), FB(764, 768, 16), N("        return false;"),
        FB(771, 771, 16), N("    return true;")]),
    dict(file=BASEF, def_re=r"^void Application::drainBaselinePivotHandovers_\(", section=True, pieces=[
        FB(813, 830, 16)]),
    dict(file=BASEF, def_re=r"^void Application::recordCommittedBaselineEpoch_\(", section=True, pieces=[
        *ba("log", "res_stats_t", "res_code_w", "res_phase_w", "res_doppler_w", "total_code_dd_w",
            "total_phase_dd_w", "total_doppler_dd_w", "total_dd_obs_w", "pivot_counts_w",
            "constellation_dd_w", "se", "rov_modeled", "em", "committed"), B(),
        G("BASE", 855, 917, "    if (!committed) return;"), FB(856, 916, 12)]),
    dict(file=BASEF, def_re=r"^bool Application::abortBaselineWithoutUpdates_\(", section=True, pieces=[
        *ba("log", "trace_ofs_w", "n_with_dd", "n_geom_ok", "n_updates", "se"),
        N("    auto& product_cache = ProductCache::getInstance();"), B(),
        FB(919, 940, 8), N("    return false;")],
        subs=[("BASE", 939, "return 1;", "return true;")]),
    dict(file=BASEF, def_re=r"^void Application::writeBaselineTraceRow_\(", section=True, pieces=[
        *ba("trace_ofs_w", "se", "built_count", "applied"), B(), FB(942, 959, 8)]),
    # --- The rover's summary and the run's.
    dict(file=BASEF, def_re=r"^void Application::closeBaselineTraceCsv_\(", section=True, pieces=[
        *ba("log", "trace_ofs_w", "trace_path", "n_updates"), B(), FB(962, 966, 4)]),
    dict(file=BASEF, def_re=r"^void Application::reportBaselineDdBuild_\(", section=True, pieces=[
        *ba("log", "bc", "synced", "rover_name", "n_with_dd", "n_geom_ok", "total_dd_obs", "total_code_dd",
            "total_phase_dd", "total_doppler_dd", "constellation_dd", "pivot_counts", "res_stats_t",
            "res_code", "res_phase", "res_doppler"), B(),
        FB(968, 1007, 4)]),
    dict(file=BASEF, def_re=r"^void Application::reportBaselineSolution_\(", section=True, pieces=[
        *ba("log", "ref_pos", "rov_apriori", "n_updates", "n_with_dd", "n_slips_ref", "n_slips_rov",
            "n_amb_resets"), B(),
        FB(1009, 1029, 4)]),
    dict(file=BASEF, def_re=r"^void Application::reportBaselineAggregate_\(", section=True, pieces=[
        *ba("log", "bc", "multi_rover", "total_rover_updates", "total_rover_dd_obs"), B(),
        FB(1049, 1063)]),
]

MEMBERS_BASE_C = {
    291: ("BaselineRunWork", "const bool multi_rover;"),
    292: ("BaselineRunWork", "size_t total_rover_updates = 0;"),
    293: ("BaselineRunWork", "size_t total_rover_dd_obs  = 0;"),
    294: ("BaselineRunWork", "size_t rovers_with_data    = 0;"),
    481: ("BaselineRoverWork", "std::ofstream trace_ofs;"),
    483: ("BaselineRoverWork", "std::filesystem::path trace_path;"),
    499: ("BaselineRoverWork", "size_t n_with_dd = 0;"),
    500: ("BaselineRoverWork", "size_t total_dd_obs = 0;"),
    501: ("BaselineRoverWork", "size_t total_code_dd = 0, total_phase_dd = 0, total_doppler_dd = 0;"),
    502: ("BaselineRoverWork", "size_t n_geom_ok = 0;"),
    503: ("BaselineRoverWork", "size_t n_updates = 0;"),
    504: ("BaselineRoverWork", "std::map<std::string, size_t> pivot_counts;"),
    505: ("BaselineRoverWork", "std::map<std::string, size_t> constellation_dd;"),
    510: ("BaselineRoverWork", "struct ResStats { double sum{ 0.0 }, sq_sum{ 0.0 }, abs_max{ 0.0 }; size_t count{ 0 }; };"),
    511: ("BaselineRoverWork", "ResStats res_code, res_phase, res_doppler;"),
    513: ("BaselineRoverWork", "double prev_tow = -1.0;"),
    514: ("BaselineRoverWork", "int    prev_week = -1;"),
    519: ("BaselineRoverWork", "double max_dur_anchor_s = 0.0;"),
    520: ("BaselineRoverWork", "bool   max_dur_anchored = false;"),
    522: ("BaselineRoverWork", "size_t n_slips_ref = 0, n_slips_rov = 0, n_amb_resets = 0;"),
    552: ("BaselineEpochWork", "std::map<std::string, std::vector<RinexObservation>>::const_iterator rov_it;"),
    553: ("BaselineEpochWork", "std::map<std::string, std::vector<RinexObservation>>::const_iterator ref_it;"),
    554: ("BaselineEpochWork", "bool both_present = false;"),
    557: ("BaselineEpochWork", "double dt = 0.0;"),
    695: ("BaselineEpochWork", "std::vector<PppObservation> rov_modeled, ref_modeled;"),
    744: ("BaselineEpochWork", "EpochMeasurement em;"),
    745: ("BaselineEpochWork", "std::set<std::string> carry_excluded;"),
    746: ("BaselineEpochWork", "size_t built_count = 0;"),
    747: ("BaselineEpochWork", "size_t applied = 0;"),
    748: ("BaselineEpochWork", "bool committed = false;"),
    749: ("BaselineEpochWork", "bool epoch_failed = false;"),
}
MEMBER_ALSO_CARRIED_BASE_C = {552, 553, 554, 557}

DROPS_BASE_C = {
    **{i: "the lambda toMinimalPppObs's head became the free function's signature"
       for i in (237, 238, 239, 240)},
    276: "the lambda's closing `};`",
    468: "the lambda eraseAmbiguitiesForSat became the member eraseBaselineAmbiguitiesForSat_",
    478: "that lambda's closing `};`",
    730: "the lambda constellationOfSatPairId became a free function",
    742: "that lambda's closing `};`",
    331: "the bare block of the overlap guard became the body of roverOverlapsReference_",
    349: "its closing brace",
    621: "the bare block of the MW pre-filter became the body of screenBaselineMwSlips_",
    665: "its closing brace",
    762: "buildBaselineDdPass_ returns false; the loop sets epoch_failed and breaks once",
    763: "(the same)",
    769: "(the same, for the catch-all handler)",
    770: "(the same)",
    812: "the bare block of the pivot-handover drain became drainBaselinePivotHandovers_",
    831: "its closing brace",
}
DUPLICATES_BASE_C = {}
LAMBDAS_BASE_C = {
    "toMinimalPppObs": (237, 276, 241, "toMinimalPppObs"),
    "eraseAmbiguitiesForSat": (468, 478, 469, "eraseBaselineAmbiguitiesForSat_"),
    "constellationOfSatPairId": (730, 742, 731, "constellationOfSatPairId"),
}
# New calls the cut adds, by the unit that makes them: the spine re-reads the aligned epochs
# synchronizeBaselineStations_ checked (a const accessor), and the early-abort diagnostic
# re-fetches the product-cache singleton the setup loaded.
ALLOWED_EXTRA_BASE_C = [
    ("runBaseline", ("getSyncedEpochs", "")),
    ("abortBaselineWithoutUpdates_", ("getInstance", "")),
]
COMMENT_EDITS_C = {
    # The file banner, rewritten to describe the cut's three levels of stages.
    "@brief Implementation of Application::runBaseline (DD baseline processing).": "file banner rewritten",
    "Kept apart from Application.cpp so that each processing mode has its": "file banner rewritten",
    "own translation unit.  The constructor, the shared helpers and the": "file banner rewritten",
    "PPP/PPP-RTK @c Application::run() entry point are in Application.cpp;": "file banner rewritten",
    "the class is declared in app/Application.hpp.": "file banner rewritten",
    "Lambda: convert a station's epoch RINEX observations into the":
        "the lambda became the function toMinimalPppObs; the line now reads `Convert a station's ...`",
    "Per-rover ambiguity erasure lambda.  Captures state_manager_":
        "the lambda became the member eraseBaselineAmbiguitiesForSat_; its banner is rewritten",
    "by reference so each rover sees its own state vector (safe": "(the same)",
    "because state_manager_ was just rebuilt above for this rover).": "(the same)",
}

STEPS_SPEC = {
    "b": dict(units=UNITS_APP + UNITS_BASE_B,
              members={"APP": MEMBERS_APP, "BASE": MEMBERS_BASE_B},
              also_carried={"APP": MEMBER_ALSO_CARRIED_APP, "BASE": set()},
              drops={"APP": DROPS_APP, "BASE": DROPS_BASE_B},
              duplicates={**DUPLICATES_APP, **DUPLICATES_BASE_B},
              lambdas={"APP": LAMBDAS_APP, "BASE": LAMBDAS_BASE_B},
              deleted={"APP": DELETED_APP, "BASE": set()},
              extra=ALLOWED_EXTRA_BASE_B,
              work_decls=["PppEpochWork"],
              new_files=[f for f in NEW_FILES]),
    "c": dict(units=UNITS_APP + UNITS_BASE_C,
              members={"APP": MEMBERS_APP, "BASE": MEMBERS_BASE_C},
              also_carried={"APP": MEMBER_ALSO_CARRIED_APP, "BASE": MEMBER_ALSO_CARRIED_BASE_C},
              drops={"APP": DROPS_APP, "BASE": DROPS_BASE_C},
              duplicates={**DUPLICATES_APP, **DUPLICATES_BASE_C},
              lambdas={"APP": LAMBDAS_APP, "BASE": LAMBDAS_BASE_C},
              deleted={"APP": DELETED_APP, "BASE": set()},
              extra=ALLOWED_EXTRA_BASE_C,
              work_decls=["PppEpochWork", "BaselineRunWork", "BaselineRoverWork", "BaselineEpochWork"],
              new_files=[f for f in NEW_FILES]),
}
COMMENT_EDITS.update(COMMENT_EDITS_C)
DEFAULT_STEP = "c"

# The working values and where each is declared.
WORK_DECLS = {
    "PppEpochWork": (WORK_HDR, r"^struct PppEpochWork\b"),
    "BaselineRunWork": (BASEF, r"^struct BaselineRunWork\b"),
    "BaselineRoverWork": (BASEF, r"^struct BaselineRoverWork\b"),
    "BaselineEpochWork": (BASEF, r"^struct BaselineEpochWork\b"),
}

# The drivers whose event list is checked: (frozen src, function regex, new unit name).
DRIVERS = [
    ("APP", r"^int Application::run\(", "run"),
    ("BASE", r"^int Application::runBaseline\(", "runBaseline"),
]

KEYWORDS = {"if", "for", "while", "switch", "catch", "return", "sizeof", "alignof", "decltype",
            "noexcept", "static_assert", "defined", "typeid", "throw", "case", "do", "else",
            "void"}  # `void (Application::*)(...)` is a member-pointer type, not a call
TEMPLATE_CALLS = {"make_unique", "make_shared"}


# ---------------------------------------------------------------------------
def git_show(rev, path):
    out = subprocess.run(["git", "-C", str(ROOT), "show", f"{rev}:{path}"],
                         capture_output=True, check=True)
    return out.stdout.decode("utf-8").replace("\r\n", "\n").lstrip("﻿")


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


def decl_parts(s):
    """(type, name) of a declaration, without const, initializer or brace-init."""
    s = norm(s).rstrip(";").strip()
    s = re.sub(r"\s*=.*$", "", s)
    s = re.sub(r"\{[^{}]*\}$", "", s).strip()
    s = re.sub(r"^const ", "", s)
    m = re.match(r"^(.*\S)\s+([A-Za-z_]\w*)$", s)
    return (m.group(1), m.group(2)) if m else (s, "")


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
        """1-based (first, last) body lines of every function at namespace scope (named or
        anonymous namespaces are entered, not taken for bodies)."""
        bodies = []
        text = "\n".join(self.blank[1:])
        depth_kinds = []
        line = 1
        i = 0
        n = len(text)
        while i < n:
            ch = text[i]
            if ch == "\n":
                line += 1
            elif ch == "{":
                before = text[max(0, text.rfind(";", 0, i), text.rfind("}", 0, i),
                                  text.rfind("{", 0, i)) + 1:i]
                is_ns = re.search(r"\bnamespace\b[\w\s:]*$", before) is not None
                if all(k == "ns" for k in depth_kinds) and not is_ns:
                    # a function body (or a class body; the frozen files hold none at
                    # namespace scope): find its close
                    depth, j, l2 = 0, i, line
                    while j < n:
                        if text[j] == "\n":
                            l2 += 1
                        elif text[j] == "{":
                            depth += 1
                        elif text[j] == "}":
                            depth -= 1
                            if depth == 0:
                                break
                        j += 1
                    bodies.append((line + 1, l2 - 1))
                    i, line = j, l2
                else:
                    depth_kinds.append("ns" if is_ns else "other")
            elif ch == "}":
                if depth_kinds:
                    depth_kinds.pop()
            i += 1
        return bodies


def expected(Fz, unit, problems):
    subs = {}
    for src, line, a, b in unit.get("subs", []):
        subs.setdefault((src, line), []).append((a, b))
    exp, covered = [], []
    for p in unit["pieces"]:
        if p[0] in ("B", "C"):
            continue
        if p[0] == "N":
            exp.append(("new", strip_comments(p[1]).strip(), None))
        elif p[0] == "G":
            _, src, s, e, text = p
            fz = Fz[src]
            if not re.match(r"^if \(.*\) \{$", fz.code[s].strip()):
                problems.append(f"{unit['def_re']}: frozen {src}:{s} is not an `if (...) {{` line")
            if fz.code[e].strip() != "}":
                problems.append(f"{unit['def_re']}: frozen {src}:{e} is not the guard's closing brace")
            covered += [(src, s), (src, e)]
            exp.append(("new", text, (src, s)))
        else:
            _, src, a, b, k = p
            fz = Fz[src]
            for i in range(a, b + 1):
                line = fz.code[i]
                if not line.strip():
                    continue
                for x, y in subs.get((src, i), []):
                    if x not in line:
                        problems.append(f"substitution {x!r} not on frozen line {src}:{i}")
                    line = line.replace(x, y)
                covered.append((src, i))
                exp.append(("frozen", unshift(line, k, f"frozen {src}:{i}").rstrip(), (src, i)))
    return exp, covered


def read_new(path):
    return (TREE / path).read_text(encoding="utf-8").replace("\r\n", "\n").lstrip("﻿")


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
            where = (f"frozen {e[2][0]}:{e[2][1]}" if e and e[0] == "frozen" else "declared")
            problems.append(
                f"{unit['def_re']} ({unit['file']}): first difference at body code line {j}:\n"
                f"       expected ({where}): {e[1] if e else '<end>'}\n"
                f"       found    ({unit['file']}:{a[0] if a else '<end>'}): {a[1] if a else '<end>'}")
            return False
    return True


# --- check 6: the event list -------------------------------------------------
def tokens(text):
    """Significant tokens of comment-stripped code: (kind, text)."""
    out = []
    for m in _CPP.finditer(text):
        s = m.group()
        if m.lastgroup == "other" and s.isspace():
            continue
        out.append((m.lastgroup, s))
    return out


def events(text, sections=frozenset()):
    """Every call in text, in order: (name, the string literal opening its arguments).  A
    member pointer &Application::name of a section counts as a call of it (a run-end table
    row)."""
    toks = tokens(text)
    ev = []
    for k, (kind, s) in enumerate(toks):
        if kind != "other" or not re.match(r"^[A-Za-z_]\w*$", s) or s in KEYWORDS:
            continue
        j = k + 1
        name = s
        if s in TEMPLATE_CALLS and j < len(toks) and toks[j][1] == "<":
            depth, j2 = 0, j
            while j2 < len(toks):
                if toks[j2][1] == "<":
                    depth += 1
                elif toks[j2][1] == ">":
                    depth -= 1
                    if depth == 0:
                        break
                j2 += 1
            name = s + "<" + "".join(t[1] for t in toks[j + 1:j2]) + ">"
            j = j2 + 1
        if j < len(toks) and toks[j][1] == "(":
            lit = []
            j += 1
            while j < len(toks) and toks[j][0] in ("str", "raw"):
                lit.append(toks[j][1])
                j += 1
            ev.append((name, "".join(lit)))
        elif (s in sections and k >= 4 and toks[k - 1][1] == ":" and toks[k - 2][1] == ":"
              and toks[k - 3][1] == "Application" and toks[k - 4][1] == "&"):
            ev.append((name, ""))
    return ev


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--step", default=DEFAULT_STEP, choices=sorted(STEPS_SPEC))
    ap.add_argument("--tree", help="read the new files from this directory (a staging copy)")
    args = ap.parse_args()
    global TREE
    if args.tree:
        TREE = pathlib.Path(args.tree).resolve()
    spec = STEPS_SPEC[args.step]
    units = spec["units"]
    Fz = {k: Frozen(v) for k, v in FROZEN.items()}
    problems = []
    cover = {}
    bodies = {}
    n_ok = 0
    for u in units:
        try:
            exp, covered = expected(Fz, u, problems)
            act, body = actual(u["file"], u["def_re"])
        except ValueError as e:
            problems.append(str(e))
            continue
        name = re.search(r"(\w+)\\\($", u["def_re"]).group(1)
        u["name"] = name
        bodies[name] = body
        for key in covered:
            cover.setdefault(key, []).append(name)
        if compare(u, exp, act, problems):
            n_ok += 1
            if args.verbose:
                print(f"OK   {name:34s} {u['file']}  "
                      f"({sum(1 for e in exp if e[0] == 'frozen')} frozen, "
                      f"{sum(1 for e in exp if e[0] == 'new')} declared code lines)")

    # 2. Coverage over every frozen function body; 4. duplicates.
    n_code = 0
    for src, fz in Fz.items():
        for i, (where, _) in spec["members"][src].items():
            if i not in spec["also_carried"][src]:
                cover.setdefault((src, i), []).append("member")
        for i in spec["drops"][src]:
            if not fz.code[i].strip():
                problems.append(f"declared drop {src}:{i} is not a code line")
            cover.setdefault((src, i), []).append("drop")
    for (src, a, b), (src2, a2, b2, why) in spec["duplicates"].items():
        da = [Fz[src].code[i].strip() for i in range(a, b + 1) if Fz[src].code[i].strip()]
        db = [Fz[src2].code[i].strip() for i in range(a2, b2 + 1) if Fz[src2].code[i].strip()]
        if da != db:
            problems.append(f"declared duplicate {src}:{a}-{b} differs from {src2}:{a2}-{b2}")
        for i in range(a, b + 1):
            if Fz[src].code[i].strip():
                cover.setdefault((src, i), []).append("duplicate")
    for src, fz in Fz.items():
        for a, b in fz.function_bodies():
            for i in range(a, b + 1):
                if not fz.code[i].strip():
                    continue
                n_code += 1
                if (src, i) not in cover:
                    problems.append(f"frozen line {src}:{i} is carried by nothing: "
                                    f"{fz.code[i].strip()[:90]}")
    twice = {k: w for k, w in cover.items() if len(w) > 1}
    for k, w in sorted(twice.items()):
        problems.append(f"frozen line {k[0]}:{k[1]} is carried twice ({', '.join(w)})")

    # 3. Members.
    pools = {}
    for struct in spec["work_decls"]:
        path, start_re = WORK_DECLS[struct]
        text = read_new(path)
        code = strip_comments(text)
        lines = code.split("\n")
        blank = blank_literals(code).split("\n")
        m = [i for i, l in enumerate(lines) if re.match(start_re, l)]
        if len(m) != 1:
            problems.append(f"struct {struct} not found once in {path}")
            continue
        o, c = brace_close(blank, m[0])
        pools[struct] = {norm(l) for l in lines[o:c + 1] if l.strip()}
    for src, members in spec["members"].items():
        for i, (struct, text) in members.items():
            if norm(text) not in pools.get(struct, set()):
                problems.append(f"member of frozen {src}:{i} not declared as `{text}` in {struct}")
            ft, fn = decl_parts(Fz[src].code[i])
            nt, nn = decl_parts(text)
            if fn != nn or (ft != nt and ft != "auto"):
                problems.append(f"member text for frozen {src}:{i} does not match the frozen "
                                f"declaration ({(ft, fn)!r} against {(nt, nn)!r})")

    # 5. Comments.
    frozen_c = Counter()
    for fz in Fz.values():
        frozen_c.update(norm(c) for c in comment_lines(fz.text))
    new_c = Counter()
    for f in spec["new_files"]:
        new_c.update(norm(c) for c in comment_lines(read_new(f)))
    edits = {norm(k) for k in COMMENT_EDITS}
    lost = [(c, n - new_c[c]) for c, n in frozen_c.items() if new_c[c] < n and c not in edits]
    for c, n in lost:
        problems.append(f"comment line lost ({n}x): {c[:100]}")

    # 6. The event lists of the drivers.
    # A driver is a call, not a section, inside another driver (run() calls runBaseline()).
    names = ({u["name"] for u in units if u.get("section") and "name" in u}
             - {uname for _, _, uname in DRIVERS})

    def expand(name, seen=()):
        """The events of unit `name`, each with the unit it comes from, sections expanded."""
        out = []
        for e in events(bodies.get(name, ""), names):
            if e[0] in names and e[0] != name and e[0] not in seen:
                out += expand(e[0], seen + (name,))
            else:
                out.append((e, name))
        return out

    n_ev = {}
    for src, def_re, uname in DRIVERS:
        fz = Fz[src]
        starts = [i for i, l in enumerate(fz.code) if re.search(def_re, l)]
        o, c = brace_close(fz.blank, starts[0])
        lams = spec["lambdas"][src]
        skip = set(spec["deleted"][src])
        lam_ev = {}
        for lname, (la, lb, lbody, _new) in lams.items():
            skip.update(range(la, lb + 1))
            lam_ev[lname] = events("\n".join(fz.code[lbody:lb]))
        text = "\n".join(fz.code[i] if i not in skip else "" for i in range(o + 1, c))
        fev = []
        for e in events(text):
            fev += lam_ev[e[0]] if e[0] in lam_ev else [e]
        nev = [e for e, unit in expand(uname) if (unit, e) not in spec["extra"]]
        n_ev[uname] = (len(fev), sum(1 for e in fev if e[0] in ("info", "warn", "error", "debug", "trace")))
        if fev != nev:
            k = next((k for k in range(min(len(fev), len(nev))) if fev[k] != nev[k]),
                     min(len(fev), len(nev)))
            problems.append(f"{uname}: event order differs at event {k} of {len(fev)}/{len(nev)}:\n"
                            f"       frozen: {fev[max(0, k - 2):k + 2]}\n"
                            f"       new   : {nev[max(0, k - 2):k + 2]}")

    for p in problems:
        print("FAIL " + p)
    if args.verbose:
        print("\nDeclared, for review:")
        for u in units:
            for p in u["pieces"]:
                if p[0] == "G":
                    print(f"  guard  {u.get('name', '?'):30s} frozen {p[1]}:{p[2]}: "
                          f"`{Fz[p[1]].code[p[2]].strip()}` -> `{p[4]}`")
            for src, line, a, b in u.get("subs", []):
                print(f"  subst  {u.get('name', '?'):30s} frozen {src}:{line}: `{a}` -> `{b}`")
        for src, d in spec["drops"].items():
            for i, r in sorted(d.items()):
                print(f"  drop   frozen {src}:{i}: {r}")
        for k, v in spec["duplicates"].items():
            print(f"  dup    frozen {k[0]}:{k[1]}-{k[2]} = {v[0]}:{v[1]}-{v[2]} ({v[3]})")
        for c, r in COMMENT_EDITS.items():
            print(f"  comment edit: {c[:60]} ({r})")
    n_members = sum(len(m) for m in spec["members"].values())
    n_drops = sum(len(d) for d in spec["drops"].values())
    print(f"\nstep U7{args.step}: {n_ok} of {len(units)} bodies verbatim in code; {n_code} frozen body "
          f"code lines, {n_members} became members, {n_drops} declared drops, "
          f"{len(spec['duplicates'])} duplicate range(s), {len(twice)} carried twice; "
          f"{sum(frozen_c.values())} frozen comment lines, {len(lost)} lost; event order "
          + ", ".join(f"{k} {v[0]} calls ({v[1]} log calls)" for k, v in n_ev.items())
          + (" equal" if not any("event order" in p for p in problems) else " DIFFERENT") + ".")
    print("RESULT", "PASS" if not problems else "FAIL")
    sys.exit(1 if problems else 0)


if __name__ == "__main__":
    main()
