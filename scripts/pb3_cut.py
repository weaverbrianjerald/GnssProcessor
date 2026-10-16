#!/usr/bin/env python3
"""Cut generator of step PB3: the PASS A slip scan and the SD-EWL row builder as named stages.

    python scripts/pb3_cut.py [--write]

Reads the two frozen functions from BASE (git) and emits, from line ranges of the frozen text
and the spec tables below:

  Application::networkPassASlipScan_  (src/core/network/driver/ApplicationNetworkAssembly.cpp)
      -> include/network/slip/PassASlipScan.hpp   + src/core/network/slip/PassASlipScan.cpp
         include/network/slip/PassASlipDrains.hpp + src/core/network/slip/PassASlipDrains.cpp
  networkAppendSdEwlRows              (src/core/network/ar/SdEwlAr.cpp)
      -> include/network/ar/SdEwlRows.hpp         + src/core/network/ar/SdEwlRows.cpp

A moved body is the frozen text between two line numbers, dedented, with bindings added at its
head.  Without --write the generator prints the line counts only.  scripts/pb3_verify.py proves
the result against the frozen text.
"""
import pathlib
import re
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from nom_cpp import lex  # noqa: E402

ROOT = HERE.parent
BASE = "deef142"
ASM = "src/core/network/driver/ApplicationNetworkAssembly.cpp"
SDE = "src/core/network/ar/SdEwlAr.cpp"


def frozen(path):
    r = subprocess.run(["git", "-C", str(ROOT), "show", f"{BASE}:{path}"], capture_output=True)
    return r.stdout.decode("utf-8").replace("\r\n", "\n").split("\n")


def used_names(text):
    toks = lex(text)
    names = {t.text for i, t in enumerate(toks)
             if t.kind == "id" and not (i > 0 and toks[i - 1].text in (".", "->", "::"))}
    for t in toks:
        if t.kind == "pp":   # a pragma's clauses name run values (num_threads, if)
            names |= set(re.findall(r"[A-Za-z_]\w*", t.text))
    return names


def indent_of(line):
    return len(line) - len(line.lstrip(" "))


RENAMES = {"Arc": "SdEwlArc", "Row": "SdEwlRow"}


def rename_code(text):
    """Applies RENAMES to identifier tokens only (comments and strings are untouched)."""
    out, last = [], 0
    for tk in lex(text):
        if tk.kind == "id" and tk.text in RENAMES:
            out.append(text[last:tk.pos])
            out.append(RENAMES[tk.text])
            last = tk.end
    out.append(text[last:])
    return "".join(out)


class Cut:
    """One frozen file and the functions cut from it."""

    def __init__(self, path, view, view_decl, aliases):
        self.path, self.L = path, frozen(path)
        self.view, self.decl, self.aliases = view, view_decl, aliases   # aliases: name -> (decl, root)

    def lines(self, a, b, expect=None):
        out = self.L[a - 1:b]
        if expect and expect not in out[0]:
            raise SystemExit(f"{self.path}:{a}: expected {expect!r}, found {out[0].strip()!r}")
        return out

    def body(self, parts, extra_names=()):
        """parts: ('L', a, b[, expect]) frozen lines | ('C', at, text) a call line at the
        indentation of frozen line `at`.  Returns (dedented text lines, used names)."""
        first = None
        for p in parts:
            if p[0] == "L":
                for ln in self.lines(p[1], p[2], p[3] if len(p) > 3 else None):
                    if ln.strip() and not ln.strip().startswith("//"):
                        first = indent_of(ln)
                        break
            if first is not None:
                break
        ded = max(first - 4, 0)
        out = []
        for p in parts:
            if p[0] == "L":
                for ln in self.lines(p[1], p[2], p[3] if len(p) > 3 else None):
                    out.append(ln[ded:] if ln[:ded].strip() == "" else ln.lstrip(" "))
            else:
                ind = indent_of(self.L[p[1] - 1]) - ded
                for k, tl in enumerate(p[2].split("\n")):
                    out.append(" " * ind + tl if k == 0 else " " * ind + tl)
        return out

    def bindings(self, body_lines):
        names = used_names("\n".join(body_lines))
        out = []
        for n in self.view:
            if n in names or (n == "pa" and any(a in names for a in self.aliases)):
                out.append(f"    {self.decl[n]} = v.{n};")
        for a, (d, root) in self.aliases.items():
            if a in names:
                out.append(f"    {d} = {root}.{a};")
        return out


def signature(ret, name, params, tail, width=100):
    """`ret name(params)tail`, wrapped at @p width with the parameters aligned under the first."""
    head = f"{ret} {name}("
    lines, cur = [], head
    for k, prm in enumerate(params):
        piece = prm + ("," if k + 1 < len(params) else ")" + tail)
        if cur.strip() and len(cur) + len(piece) + 1 > width and cur != head:
            lines.append(cur.rstrip())
            cur = " " * len(head)
        cur += piece + " "
    lines.append(cur.rstrip())
    return chr(10).join(lines)


def func_text(cut, spec, vname="v"):
    name, ret, params, parts = spec["name"], spec.get("ret", "void"), spec["params"], spec["parts"]
    body = cut.body(parts)
    binds = spec.get("binds")
    binds = cut.bindings(body) if binds is None else binds
    head = signature(ret, name, params, " {")
    txt = head.split(chr(10)) + binds + ([""] if binds else []) + body
    while txt and txt[-1].strip() == "":
        txt.pop()
    txt.append("}")
    return "\n".join(txt)


def doc(brief, params=(), indent=""):
    out = [f"{indent}/**", f"{indent} * @brief {brief[0]}"]
    for ln in brief[1:]:
        out.append(f"{indent} *        {ln}")
    for p in params:
        out.append(f"{indent} * @param {p}")
    out.append(f"{indent} */")
    return "\n".join(out)


def decl_text(spec):
    return signature(spec.get("ret", "void"), spec["name"], spec["params"], ";")


# =============================================================================================
# The PASS A slip scan
# =============================================================================================
VIEW_ORDER = ["se", "ctx", "traces", "config_", "log", "dt", "pl_now_s", "carry_warm", "stations",
              "modeled", "present", "shared_phase_keys", "carry_slipped_ambs", "qc", "slip_ev",
              "n_sta_total", "parallel_on", "n_threads", "phase_datum_prioritized",
              "carry_invariant", "trace_flag_rates", "ar_suspect_armed", "arc_rows", "fr_if_slip",
              "cbadm_admit", "i_lo", "pa", "sid_screens", "pa_drops"]
VIEW_BIND = {
    "se": "const SyncedEpoch& se", "ctx": "NetworkRunContext& ctx",
    "traces": "NetworkRunTraces& traces", "config_": "const GnssConfig& config_",
    "log": "auto log", "dt": "double dt", "pl_now_s": "double pl_now_s",
    "carry_warm": "bool carry_warm", "stations": "auto& stations",
    "modeled": "std::vector<std::vector<PppObservation>>& modeled",
    "present": "std::vector<bool>& present",
    "shared_phase_keys": "const std::set<std::string>& shared_phase_keys",
    "carry_slipped_ambs": "std::set<std::pair<int, std::string>>& carry_slipped_ambs",
    "qc": "SlipQcChannel& qc", "slip_ev": "EpochEvents::Slip& slip_ev",
    "n_sta_total": "const int n_sta_total", "parallel_on": "const bool parallel_on",
    "n_threads": "const int n_threads",
    "phase_datum_prioritized": "const bool phase_datum_prioritized",
    "carry_invariant": "const bool carry_invariant", "trace_flag_rates": "const bool trace_flag_rates",
    "ar_suspect_armed": "const bool ar_suspect_armed", "arc_rows": "net::ArcTable& arc_rows",
    "fr_if_slip": "auto& fr_if_slip", "cbadm_admit": "const std::set<std::string>* cbadm_admit",
    "i_lo": "const int i_lo", "pa": "PassABuffers& pa", "sid_screens": "const bool sid_screens",
    "pa_drops": "std::vector<std::map<std::string, DropReason>>& pa_drops",
}
VIEW_MEMBER = {  # header member declarations
    "se": "const SyncedEpoch& se;                        ///< the synced epoch",
    "ctx": "NetworkRunContext& ctx;                       ///< run context: knobs, counters, carry, entities",
    "traces": "NetworkRunTraces& traces;                   ///< trace streams and the event writer",
    "config_": "const GnssConfig& config_;                   ///< run configuration",
    "log": "const std::shared_ptr<spdlog::logger>& log;  ///< the main log",
    "dt": "double dt;                                   ///< epoch step (s)",
    "pl_now_s": "double pl_now_s;                             ///< phase-leveling clock (s)",
    "carry_warm": "bool carry_warm;                            ///< the carry is past its warm-up",
    "stations": "std::vector<NetStation>& stations;          ///< the station roster",
    "modeled": "std::vector<std::vector<PppObservation>>& modeled;  ///< per-station modelled rows (written by the scan)",
    "present": "std::vector<bool>& present;                  ///< station present this epoch (written by the fold)",
    "shared_phase_keys": "const std::set<std::string>& shared_phase_keys;  ///< legacy-datum shared phase-bias keys",
    "carry_slipped_ambs": "std::set<std::pair<int, std::string>>& carry_slipped_ambs;  ///< raw-detector slip funnel (written)",
    "qc": "SlipQcChannel& qc;                           ///< slip-QC channel (raw observables, history, counters)",
    "slip_ev": "EpochEvents::Slip& slip_ev;                 ///< the epoch's slip events (written)",
    "n_sta_total": "int n_sta_total;                            ///< station count",
    "parallel_on": "bool parallel_on;                           ///< the scan runs under OpenMP",
    "n_threads": "int n_threads;                              ///< OpenMP thread count",
    "phase_datum_prioritized": "bool phase_datum_prioritized;               ///< prioritized phase datum (no special pivot)",
    "carry_invariant": "bool carry_invariant;                        ///< invariant-carry vehicle",
    "trace_flag_rates": "bool trace_flag_rates;                       ///< flag-rate tier counters armed",
    "ar_suspect_armed": "bool ar_suspect_armed;                       ///< the AR suspect-corner mask is armed",
    "arc_rows": "net::ArcTable& arc_rows;                    ///< the leveling carry",
    "fr_if_slip": "FlagRate& fr_if_slip;                       ///< IF-phase flag-rate counter",
    "cbadm_admit": "const std::set<std::string>* cbadm_admit;     ///< run-latched admitted code keys (null when unarmed)",
    "i_lo": "int i_lo;                                   ///< first station of the scan (0 prioritized, else 1)",
    "pa": "PassABuffers& pa;                           ///< per-station buffers the scan fills and the drains read",
    "sid_screens": "bool sid_screens;                           ///< the drop-reason writers are armed",
    "pa_drops": "std::vector<std::map<std::string, DropReason>>& pa_drops;  ///< per-station drop reasons (armed only)",
}
ALIASES = {  # frozen alias statements dropped from the orchestrator and rebuilt in the stages
    "a_ok": ("auto& a_ok", "pa"), "a_err": ("auto& a_err", "pa"),
    "sta_repairs": ("auto& sta_repairs", "pa"), "sta_if_resets": ("auto& sta_if_resets", "pa"),
    "sta_mw_refloat": ("auto& sta_mw_refloat", "pa"),
}
V = "const PassAView& v"
RELEVEL = ["RelevelEpochState* relevel_ptr", "std::map<net::ArcTuple, RelevelPend>& relevel_pend"]
CBPTR = "const CommonBandEpoch* common_band_ptr"
MWRESULTS = "const std::vector<MwSlipResult>& mwr"
PAIROK = "const PassAMwPairOk& mw_pair_ok"

SCAN = [  # PassASlipScan.{hpp,cpp}
    dict(name="passAScanStations", params=[V],
         brief=["Per-station PASS A tail, parallel over stations when enabled.",
                "Models each station, allocates its shared states, and runs its MW and IF-phase",
                "slip detectors. Everything written is station-local or an index-addressed slot, so",
                "the loop is safe under OpenMP; present[] and exceptions are folded serially by the",
                "stages that follow."],
         parts=[("L", 222, 230), ("L", 250, 277), ("C", 278, "passAScanMw(v, i, st);"), ("L", 408, 426)]),
    dict(name="passAScanMw", params=[V, "int i", "NetStation& st"],
         brief=["Rolls one station's MW arcs forward and collects either TurboEdit integer repairs",
                "or, under mw_detect, the admitted refloat candidates. A detection failure costs only",
                "this station's MW action for the epoch."],
         parts=[("L", 278, 287), ("C", 288, "passAMwDetectOnly(v, i, st, mwr);"), ("L", 405, 407)]),
    dict(name="passAMwDetectOnly", params=[V, "int i", "NetStation& st", MWRESULTS],
         brief=["The detect-only MW branch: fixes the admission knobs and the pair scope, admits",
                "the flagged pairs, then records the common-band evidence from the same detector."],
         parts=[("L", 288, 335), ("C", 336, "passAMwAdmit(v, i, mwr, mw_pair_ok, frac_max, ev_on, ev_ratio, ev_narc);"),
                ("C", 363, "passAMwEvidence(v, i, st, mwr, mw_pair_ok);")]),
    dict(name="passAMwAdmit", params=[V, "int i", MWRESULTS, PAIROK, "const double frac_max",
                                      "const bool ev_on", "const double ev_ratio", "const int ev_narc"],
         brief=["Admits each MW flag of one station as a refloat candidate, or counts why it was",
                "not. Writes the station's index-addressed census and candidate slots only."],
         parts=[("L", 336, 361)]),
    dict(name="passAMwEvidence", params=[V, "int i", "NetStation& st", MWRESULTS, PAIROK],
         brief=["Collects one station's common-band scope evidence: every arc the detector",
                "processed, every flag, and the receiver's own loss-of-lock bits. Collected whether",
                "or not the scope is armed, so the census sizes the mechanism before it is."],
         parts=[("L", 363, 404)]),
    dict(name="passAFoldPresent", params=[V],
         brief=["Commits present[] from the scan's per-station flags and logs a station whose",
                "geometry threw, in station order."],
         parts=[("L", 428, 437)]),
    dict(name="passAFoldDropReasons", params=[V],
         brief=["Folds the per-station drop reasons (elevation mask, no model) into the arc",
                "ledger, in station order. A station that threw is left out: its whole epoch is",
                "missing, which is an outage and not a per-satellite screen."],
         parts=[("L", 439, 454)]),
    dict(name="passASecondCode", params=[V],
         brief=["Second-code-channel bookkeeping for the epoch: republishes each station's",
                "companion cell set for PASS B, counts the cells, both cell edges and the receiver",
                "inter-signal states. Serial and station-ordered; skipped entirely when off."],
         parts=[("L", 456, 549)]),
    dict(name="passASecondPhase", params=[V],
         brief=["Second-phase-channel bookkeeping for the epoch: republishes each station's",
                "second-phase cell set for PASS B, counts cells, edges and arcs, and advances the",
                "between-channel witness. Serial and station-ordered; skipped entirely when off."],
         parts=[("L", 551, 595), ("C", 600, "passASecondPhaseWitness(v, i, st, ob, C, allow, now_ep);"),
                ("L", 690, 726)]),
    dict(name="passASecondPhaseWitness",
         params=[V, "int i", "NetStation& st", "const PppObservation& ob",
                 "NetworkServiceCounters::SecondPhase& C", "const std::vector<std::string>& allow", "const long long now_ep"],
         brief=["Per-observation part of the second-phase bookkeeping: counts the fallback",
                "occurrences against the station's latched phase and advances the epoch-to-epoch",
                "witness of (latched minus second) phase, per band."],
         parts=[("L", 596, 689)]),
    dict(name="passARawObservables", params=[V],
         brief=["Computes the shared raw slip observables once per epoch, after present[] is final",
                "and before the drains and the observability pre-filter, then qualifies them and",
                "advances the CS-GF authority. Skipped when no consumer asked for them."],
         parts=[("L", 728, 811), ("C", 819, "passAIonoCsAdvance(v);"), ("L", 844, 844)]),
    dict(name="passAIonoCsAdvance", params=[V],
         brief=["One CS-GF level-authority recursion step per dual-band line of sight from this",
                "epoch's qualified verdicts. Serial and station-ordered, so deterministic."],
         parts=[("L", 812, 843)]),
    dict(name="passADatumStationMwDetect", params=[V, "const bool datum_detect_on"],
         brief=["MW detection for the legacy datum station, which the parallel scan skips.",
                "Detector-mode candidates only; the pivot's ambiguities are gauge-pinned, so no",
                "integer repair applies. Does nothing unless datum_detect_on."],
         parts=[("L", 846, 853), ("L", 857, 906)]),
    dict(name="passAFoldMwAdmission", params=[V],
         brief=["Reduces the per-station MW admission slots into the run counters, in station",
                "order, and stamps the AR search's suspect corners. Runs unconditionally: these",
                "counters are the mechanism's knob-independent telemetry."],
         parts=[("L", 908, 929)]),
    dict(name="passAResumeSlipWarmup", params=[V, "bool& mw_carry_warm"],
         brief=["Resume slip warm-up: for the first epochs after a warm resume the MW/LLI drain",
                "runs diagnostic-only, so mw_carry_warm is cleared and the retirement is logged."],
         parts=[("L", 946, 955), ("L", 957, 965)]),
    dict(name="passACommonBandVerdict", params=[V, "CommonBandEpoch& common_band"],
         brief=["Forms the common-band scope verdict for the epoch, before either drain, and adds",
                "its tallies to the run counters. The verdict is formed armed or not."],
         parts=[("L", 966, 972), ("L", 974, 1002)]),
]
DRAINS = [  # PassASlipDrains.{hpp,cpp}
    dict(name="passAMwRefloatStage",
         params=[V, "bool mw_carry_warm", "const bool datum_detect_on", RELEVEL[0], CBPTR],
         ret="std::map<net::ArcTuple, RelevelPend>", ret_name="relevel_pend",
         brief=["MW-detect / LLI refloat drain with the datum re-reference queues and the bridge",
                "pends it works on. Returns the epoch's bridge pends, which the later drains keep",
                "filling and the driver writes back."],
         parts=[("L", 1005, 1039)]),
    dict(name="passAIfTdStage", params=[V, RELEVEL[0], RELEVEL[1], CBPTR],
         brief=["IF-phase triple-difference slip driver."],
         parts=[("L", 1041, 1065)]),
    dict(name="passAGfArcJumpStage", params=[V, RELEVEL[0], RELEVEL[1], CBPTR],
         brief=["Arc-scoped GF jump trigger, then the AR search's suspect-corner stamps for every",
                "GF flag."],
         parts=[("L", 1067, 1088)]),
    dict(name="passAWindupRestartStage", params=[V, RELEVEL[0], RELEVEL[1]],
         brief=["Wind-up accumulator-restart response: refloats the arcs whose accumulator",
                "restarted this epoch."],
         parts=[("L", 1090, 1108)]),
    dict(name="passAGapReentryStage", params=[V, RELEVEL[0], RELEVEL[1]],
         brief=["Gap re-entry refloat, row-level then per signal, over the arc ledger's",
                "re-entry population when the ledger is armed."],
         parts=[("L", 1110, 1185)]),
    dict(name="passAInjectedFreeStage", params=[V, RELEVEL[0], RELEVEL[1]],
         brief=["Injected free_epoch arc response of the event harness. Unreachable unless the",
                "injector staged a free_epoch directive."],
         parts=[("L", 1187, 1200)]),
]
VIEW_INIT = ("const PassAView v{" + ", ".join(VIEW_ORDER) + "};")


def wrap_init(text, indent=4, width=96):
    words, lines, cur = text.split(" "), [], " " * indent
    for w in words:
        if len(cur) + len(w) + 1 > width and cur.strip():
            lines.append(cur.rstrip())
            cur = " " * (indent + 4)
        cur += w + " "
    lines.append(cur.rstrip())
    return "\n".join(lines)


# =============================================================================================
# The SD-EWL row builder
# =============================================================================================
SD_ORDER = ["config_", "st", "est", "sta_obs", "h2local", "t", "A_s", "A_r", "w", "y", "tags",
            "trace_rows", "fix", "station_index", "cfg", "ctr", "fix_on", "tr2", "tr"]
SD_BIND = {
    "config_": "const GnssConfig& config_", "st": "NetStation& st",
    "est": "const NetworkEstimator& est", "sta_obs": "const std::vector<PppObservation>& sta_obs",
    "h2local": "const std::map<int, int>& h2local", "t": "const GnssTime& t",
    "A_s": "Eigen::MatrixXd& A_s", "A_r": "Eigen::MatrixXd& A_r", "w": "Eigen::VectorXd& w",
    "y": "Eigen::VectorXd& y", "tags": "std::vector<RowTag>& tags",
    "trace_rows": "std::vector<std::string>& trace_rows", "fix": "const net::StationTable* fix",
    "station_index": "std::size_t station_index", "cfg": "const auto& cfg", "ctr": "auto& ctr",
    "fix_on": "const bool fix_on", "tr2": "const auto& tr2", "tr": "const auto& tr",
}
SD_MEMBER = {
    "config_": "const GnssConfig& config_;", "st": "NetStation& st;",
    "est": "const NetworkEstimator& est;", "sta_obs": "const std::vector<PppObservation>& sta_obs;",
    "h2local": "const std::map<int, int>& h2local;   ///< state index -> local A_r column of this epoch's phase rows",
    "t": "const GnssTime& t;", "A_s": "Eigen::MatrixXd& A_s;", "A_r": "Eigen::MatrixXd& A_r;",
    "w": "Eigen::VectorXd& w;", "y": "Eigen::VectorXd& y;", "tags": "std::vector<RowTag>& tags;",
    "trace_rows": "std::vector<std::string>& trace_rows;", "fix": "const net::StationTable* fix;",
    "station_index": "std::size_t station_index;", "cfg": "const SdEwlConfig& cfg;",
    "ctr": "NetStation::SdEwlCounters& ctr;", "fix_on": "bool fix_on;",
    "tr2": "SdEwlTraceFull tr2;   ///< trace row, full form (with the integer-path columns)",
    "tr": "SdEwlTraceShort tr;   ///< trace row, float-path form",
}
SD_VIEW_INIT = "const SdEwlRowView v{" + ", ".join(SD_ORDER) + "};"
SD_PBCB = "std::map<std::pair<std::string, std::string>, int>"
SD_STAGES = [
    dict(name="sdEwlFillBiasStates", params=[f"{V.replace('PassAView', 'SdEwlRowView')}",
                                              f"{SD_PBCB}& pb_state", f"{SD_PBCB}& cb_state"],
         brief=["Fills the station's shared phase-bias and code-bias state indices, keyed by",
                "(satellite, signal). A column another station registered has no state here and",
                "reads as absent, which matches the real rows (the innovations omit it)."],
         parts=[("L", 854, 861)]),
    dict(name="sdEwlFillCandidates", params=[V.replace("PassAView", "SdEwlRowView"), "SdEwlCandidates& cand"],
         brief=["Fills the station's per-(satellite, band) ambiguity candidates from",
                "StateManager's own key map, each list sorted ascending by signal name so the pair",
                "search that follows is a strict total order (byte identity)."],
         parts=[("L", 868, 877, "// ---"), ("L", 880, 892)]),
    dict(name="sdEwlResolveArcs",
         params=[V.replace("PassAView", "SdEwlRowView"), "const Eigen::VectorXd& x_lin",
                 f"const {SD_PBCB}& pb_state", f"const {SD_PBCB}& cb_state",
                 "const SdEwlCandidates& cand", "std::map<char, std::vector<SdEwlArc>>& by_const"],
         brief=["Resolves every eligible satellite of the station into an arc, grouped by",
                "constellation: elects the phase pair, checks the wavelength and code channels,",
                "attaches the published integer, and linearizes against the station's x_lin. Each",
                "skip is counted and traced with its cause."],
         head=[(786, 793), (846, 848), (862, 866)],
         parts=[("L", 894, 901), ("L", 903, 1084)]),
    dict(name="sdEwlEmitGroup",
         params=[V.replace("PassAView", "SdEwlRowView"), "const std::vector<const SdEwlArc*>& g",
                 "bool fixed_group"],
         brief=["Builds one whitened group of rows against the group's pivot arc: gates each",
                "row, caps the count, appends the design, right-hand side and tags to the block,",
                "and traces every outcome. Unwinds the append if the factorization fails."],
         parts=[("L", 1086, 1095), ("L", 1097, 1109), ("L", 1111, 1173),
                ("C", 1174, "sdEwlAppendRows(v, p, rows, fixed_group);")]),
    dict(name="sdEwlAppendRows",
         params=[V.replace("PassAView", "SdEwlRowView"), "const SdEwlArc& p",
                 "const std::vector<SdEwlRow>& rows", "bool fixed_group"],
         brief=["Appends one group's rows to the station block. The rows share the pivot arc's",
                "noise, so R = diag(var_s) + var_p * 11^T; they are whitened by L^-1 into",
                "unit-information rows and the append is unwound if the factorization fails."],
         parts=[("L", 1174, 1252)]),
    dict(name="sdEwlBuildGroups",
         params=[V.replace("PassAView", "SdEwlRowView"), "std::map<char, std::vector<SdEwlArc>>& by_const"],
         brief=["Partitions each constellation's arcs into the fixed and float groups (one group",
                "when the integer path is off) and emits them. The pivot is the most mature arc,",
                "ties by satellite name, so the row set is deterministic."],
         forwarder="const auto emitGroup = [&](const std::vector<const SdEwlArc*>& g, bool fixed_group) {\n"
                   "    sdEwlEmitGroup(v, g, fixed_group);\n};",
         parts=[("L", 1255, 1311)]),
]




# =============================================================================================
# Emission
# =============================================================================================
def read_work(rel):
    return (ROOT / rel).read_bytes().decode("utf-8").replace("\r\n", "\n")


def write_work(rel, text):
    (ROOT / rel).write_bytes(text.replace("\n", "\r\n").encode("utf-8"))


def splice(rel, first, last, new_lines, base_lines):
    """Replace frozen lines [first, last] of the (unmodified) working file by new_lines."""
    cur = read_work(rel).split("\n")
    if cur != base_lines:
        raise SystemExit(f"{rel}: the working file is not the frozen text; refusing to splice")
    return "\n".join(cur[:first - 1] + new_lines + cur[last:])


SLIP_INCLUDES = """#include "network/slip/PassASlipScan.hpp"

#include "network/slip/SlipQc.hpp"            // PassABuffers, the MW admission and the drain seams
#include "network/slip/RawObservables.hpp"    // the raw slip observables and the CS-GF recursion
#include "network/slip/CommonBandScope.hpp"   // CommonBandEpoch, CommonBandProcessed
#include "network/assembly/SignalRoster.hpp"  // the roster's modelled role reads
#include "network/assembly/ExtraBandScope.hpp"   // the pin bands and the primary-pair predicate
#include "network/assembly/SecondCodeScope.hpp"  // secondCodeCellEdges
#include "network/assembly/SecondPhaseScope.hpp" // secondPhaseCellEdges, secondPhaseWitness
#include "network/assembly/Responses.hpp"     // prmRespond
#include "network/state/ArcLedger.hpp"        // arcReasonExpand, the re-entry population
#include "network/state/Entities.hpp"         // net::putLos and the entity tables
#include "network/state/RunContext.hpp"       // the run context and its counters
#include "network/ar/ArSuspect.hpp"           // arSuspectNote
#include "network/diag/RunTraces.hpp"         // NetworkRunTraces::emitEvent
#include "filter/StateKeySchema.hpp"          // StateKeys
#include "slip/MwSlipDetector.hpp"            // the station MW detector
#include "slip/TurboEditResolver.hpp"         // TurboEdit integer repair
#include "slip/IfPhaseSlipDetector.hpp"       // IF-phase triple-difference detection
#include "config/GnssConfig.hpp"
#include "common/Constants.hpp"
#include "common/GnssIdentity.hpp"            // systemOfSat, letterOfSat, ObservableId, obsTypeOf
#include "rinex/MultiRinexSync.hpp"           // SyncedEpoch

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>
"""

SLIP_HPP_HEAD = """#pragma once
/**
 * @file PassASlipScan.hpp
 * @brief The PASS A slip scan of one network epoch as named stages: the parallel station
 *        scan with its MW and IF-phase detectors, the serial folds, the second code and
 *        second phase bookkeeping, the shared raw observables, and the MW census.
 *
 * `Application::networkPassASlipScan_` (ApplicationNetworkAssembly.cpp) builds a
 * @ref PassAView once per epoch and calls the stages in order. A stage reads the view,
 * binds the names it uses at its head, and keeps the body of the scan it was cut from.
 * The serial response drains that follow are in PassASlipDrains.hpp.
 *
 * Threading: passAScanStations and the stages it calls (passAScanMw, passAMwDetectOnly,
 * passAMwAdmit, passAMwEvidence) run inside an OpenMP region and write only
 * index-addressed per-station slots. Every other stage is serial and station-ordered.
 */

#include "network/slip/SlipQc.hpp"
#include "network/slip/CommonBandScope.hpp"
#include "network/driver/Epoch.hpp"
#include "network/driver/RunTypes.hpp"
#include "network/diag/RunTraces.hpp"
#include "network/state/Entities.hpp"
#include "network/state/RunContext.hpp"
#include "slip/MwSlipTypes.hpp"

#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace spdlog { class logger; }

namespace GnssProcessor {

class GnssConfig;
struct SyncedEpoch;

/// The admission predicate of the MW detect branch: whether a pair is in the configured scope.
using PassAMwPairOk = std::function<bool(const std::string&, const std::string&, const std::string&)>;

/**
 * @brief Handles on the run and the epoch that every PASS A stage reads.
 *
 * References and copies only; owns nothing. The members are the names the scan used as
 * locals, so a stage body keeps its original spelling after the bindings at its head.
 */
struct PassAView {
"""

DRAINS_HPP_HEAD = """#pragma once
/**
 * @file PassASlipDrains.hpp
 * @brief The serial response drains of the PASS A slip scan as named stages: the MW refloat
 *        with the datum re-reference queues, the IF-phase driver, the arc-scoped GF jump,
 *        the wind-up restart, the gap re-entry pair and the injected free_epoch response.
 *
 * Each stage wraps one drain seam of SlipQc.hpp or GfArcJump.hpp with the explanation of
 * why it sits at its place in the epoch. The bridge pends, the re-level state pointer and
 * the common-band pointer are the values the drains share; they are explicit parameters.
 * Called in order by `Application::networkPassASlipScan_`.
 */

#include "network/slip/PassASlipScan.hpp"

namespace GnssProcessor {

"""


def decl_block(specs):
    return "\n".join(doc(s["brief"]) + "\n" + decl_text(s) + "\n" for s in specs)


def build_slip(asm, texts):
    A = lambda a, b: asm.L[a - 1:b]   # noqa: E731
    o = []
    o += A(181, 221)
    o += A(231, 233)
    o += A(239, 249)
    o += ["    // The handles the stages read, built once per epoch."] + wrap_init(VIEW_INIT).split(chr(10)) + [""]
    o += ["    // The stages, in epoch order. Each binds the names it uses from the view."]
    o += ["    passAScanStations(v);       // models, shared states, MW and IF-phase detection (parallel)",
          "    passAFoldPresent(v);        // present[] and the thrown-station log, serial",
          "    passAFoldDropReasons(v);    // elevation-mask and no-model reasons into the arc ledger",
          "    passASecondCode(v);         // second code channel cells, edges and counters",
          "    passASecondPhase(v);        // second phase channel cells, edges, counters and witness",
          "    passARawObservables(v);     // shared raw slip observables, qualification, CS-GF authority"]
    o += A(854, 856)
    o += ["    passADatumStationMwDetect(v, datum_detect_on);",
          "    passAFoldMwAdmission(v);    // MW admission census into the run counters", ""]
    o += A(931, 936)
    o += A(937, 945)
    o += A(956, 956)
    o += ["    passAResumeSlipWarmup(v, mw_carry_warm);"]
    o += A(973, 973)
    o += ["    passACommonBandVerdict(v, common_band);"]
    o += A(1003, 1004)
    o += ["    std::map<net::ArcTuple, RelevelPend> relevel_pend =",
          "        passAMwRefloatStage(v, mw_carry_warm, datum_detect_on, relevel_ptr, common_band_ptr);",
          "    passAIfTdStage(v, relevel_ptr, relevel_pend, common_band_ptr);",
          "    passAGfArcJumpStage(v, relevel_ptr, relevel_pend, common_band_ptr);",
          "    passAWindupRestartStage(v, relevel_ptr, relevel_pend);",
          "    passAGapReentryStage(v, relevel_ptr, relevel_pend);",
          "    passAInjectedFreeStage(v, relevel_ptr, relevel_pend);"]
    o += A(1201, 1202)
    members = "\n".join("    " + VIEW_MEMBER[n] for n in VIEW_ORDER)
    scan_hpp = SLIP_HPP_HEAD + members + "\n};\n\n" + decl_block(SCAN) + "\n} // namespace GnssProcessor\n"
    drains_hpp = DRAINS_HPP_HEAD + decl_block(DRAINS) + "\n} // namespace GnssProcessor\n"
    banner = (" * The bodies were cut verbatim from `Application::networkPassASlipScan_`; only the\n"
              " * bindings at the head of each stage and the calls between stages are new.\n */\n\n")
    scan_cpp = ("/**\n * @file PassASlipScan.cpp\n * @brief The PASS A slip scan stages declared in PassASlipScan.hpp.\n *\n"
                + banner + SLIP_INCLUDES + "\nnamespace GnssProcessor {\n\n"
                + "\n\n".join(texts["scan_defs"]) + "\n\n} // namespace GnssProcessor\n")
    drains_cpp = ("/**\n * @file PassASlipDrains.cpp\n * @brief The PASS A response drain stages declared in PassASlipDrains.hpp.\n *\n"
                  + banner + '#include "network/slip/PassASlipDrains.hpp"\n\n'
                  '#include "network/slip/GfArcJump.hpp"          // networkSlipDrainGfArcJump\n'
                  '#include "network/slip/SlipQc.hpp"             // the drain seams\n'
                  '#include "network/assembly/Responses.hpp"     // prmRespond, ResponseSite\n'
                  '#include "network/assembly/SignalRoster.hpp"  // signalRosterModelledArcs\n'
                  '#include "network/state/ArcLedger.hpp"        // ArcReentryEpoch, the re-entry population\n'
                  '#include "network/state/Entities.hpp"         // net::relevelPendMap, net::assignSatFlags\n'
                  '#include "network/ar/ArSuspect.hpp"           // arSuspectNote\n'
                  '#include "network/carry/Inject.hpp"           // the injected free_epoch directive\n'
                  '#include "config/GnssConfig.hpp"\n\n'
                  "#include <map>\n#include <set>\n#include <string>\n\nnamespace GnssProcessor {\n\n"
                  + "\n\n".join(texts["drain_defs"]) + "\n\n} // namespace GnssProcessor\n")
    return o, scan_hpp, scan_cpp, drains_hpp, drains_cpp


SD_HPP_HEAD = """#pragma once
/**
 * @file SdEwlRows.hpp
 * @brief The SD pre-reduce EWL row builder as named stages: the correction-frame state
 *        indices, the ambiguity candidates, the arc resolution, and the whitened groups.
 *
 * `networkAppendSdEwlRows` (SdEwlAr.cpp, declared in SdEwlAr.hpp) builds a
 * @ref SdEwlRowView once per station-epoch and calls the stages in order. The algebra, the
 * correction frame and the thread-safety contract are documented at that declaration; a
 * stage here only owns one step of it. Everything written is the station's own block.
 */

#include "network/driver/RunTypes.hpp"
#include "network/assembly/Contributions.hpp"
#include "network/solve/Estimator.hpp"
#include "network/state/Entities.hpp"
#include "config/GnssConfig.hpp"
#include "common/GnssTypes.hpp"

#include <Eigen/Core>

#include <functional>
#include <map>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace GnssProcessor {

/// Trace row sink, full form: the status, the satellites and signals, the arc statistics and
/// the three trailing integer-path columns.
using SdEwlTraceFull = std::function<void(
    const char*, const std::string&, const std::string&, const std::string&, const std::string&,
    const std::string&, const std::string&, std::size_t, std::size_t, bool, double, double, double,
    double, double, long long, bool, bool)>;

/// Trace row sink, float-path form (the integer-path columns are zero).
using SdEwlTraceShort = std::function<void(
    const char*, const std::string&, const std::string&, const std::string&, const std::string&,
    const std::string&, const std::string&, std::size_t, std::size_t, bool, double, double, double,
    double, double)>;

/// One ambiguity candidate of a (satellite, band): the signal, its A_r local column, and its
/// global state index.
using SdEwlCand = std::tuple<std::string, int, int>;

/// The station's candidates by (satellite, band digit), each list ascending by signal name.
using SdEwlCandidates = std::map<std::pair<std::string, char>, std::vector<SdEwlCand>>;

"""


def build_sd(sde, texts):
    A = lambda a, b: sde.L[a - 1:b]   # noqa: E731
    arc = [ln[4:] for ln in A(797, 828)]
    arc[0] = arc[0].replace("struct Arc", "struct SdEwlArc")
    row = [A(1110, 1110)[0].strip().replace("struct Row", "struct SdEwlRow").replace("const Arc*", "const SdEwlArc*")]
    o = []
    o += A(731, 785)
    o += ["    // The handles the stages read, built once per station-epoch."] + wrap_init(SD_VIEW_INIT).split(chr(10)) + [""]
    o += A(830, 845)
    o += A(849, 853)
    o += ["    sdEwlFillBiasStates(v, pb_state, cb_state);", ""]
    o += ["    // Ambiguity candidates per (satellite, band), then the arcs by constellation and the rows."]
    o += A(878, 879)
    o += ["    sdEwlFillCandidates(v, cand);", ""]
    o += [A(902, 902)[0].replace("<Arc>", "<SdEwlArc>")]
    o += ["    sdEwlResolveArcs(v, x_lin, pb_state, cb_state, cand, by_const);",
          "    sdEwlBuildGroups(v, by_const);", "}"]
    members = "\n".join("    " + SD_MEMBER[n] for n in SD_ORDER)
    hpp = (SD_HPP_HEAD
           + "/// One resolved arc: the zero-difference-consistent EWL statistic plus the six columns the row\n"
           + "/// needs (two local ambiguities, and the shared PB/CB pair per band).\n"
           + "\n".join(arc) + "\n\n"
           + "/// One gated row of a group: the satellite arc, its correction-frame right-hand side and variance.\n"
           + "\n".join(row) + "\n\n"
           + "/**\n * @brief Handles on one station-epoch of the SD-EWL row builder that every stage reads.\n *\n"
           + " * References and copies only; owns nothing. The members are the parameters of\n"
           + " * `networkAppendSdEwlRows` and the values its prelude derived from them.\n */\n"
           + "struct SdEwlRowView {\n" + members + "\n};\n\n"
           + decl_block(SD_STAGES) + "\n} // namespace GnssProcessor\n")
    cpp = ("/**\n * @file SdEwlRows.cpp\n * @brief The SD pre-reduce EWL row stages declared in SdEwlRows.hpp.\n *\n"
           " * The bodies were cut verbatim from `networkAppendSdEwlRows`; only the bindings at the\n"
           " * head of each stage and the calls between stages are new.\n */\n\n"
           '#include "network/ar/SdEwlRows.hpp"\n\n'
           '#include "network/ar/SdEwlAr.hpp"             // sdEwlBands, SdEwlFix\n'
           '#include "network/state/RunContext.hpp"\n'
           '#include "common/Constants.hpp"\n'
           '#include "common/GnssIdentity.hpp"            // systemOfSat, letterOfSat, ObservableId\n'
           '#include "filter/StateKeySchema.hpp"           // StateKeys\n'
           '#include "filter/StateManager.hpp"\n'
           '#include "slip/MwSlipDetector.hpp"             // MwArcZdMean, arcZdMean\n\n'
           "#include <fmt/format.h>\n\n#include <algorithm>\n#include <cmath>\n#include <map>\n"
           "#include <set>\n#include <string>\n#include <tuple>\n#include <utility>\n#include <vector>\n\n"
           "namespace GnssProcessor {\n\n" + "\n\n".join(texts["sd_defs"]) + "\n\n} // namespace GnssProcessor\n")
    return o, hpp, cpp


def emit_all(asm, sde, texts):
    o, scan_hpp, scan_cpp, drains_hpp, drains_cpp = build_slip(asm, texts)
    new_asm = splice(ASM, 181, 1202, o, asm.L)
    inc = '#include "network/slip/SlipQc.hpp"       // slip-QC pure helpers'
    assert inc in new_asm
    new_asm = new_asm.replace(inc, '#include "network/slip/PassASlipScan.hpp"   // the PASS A scan stages\n'
                              '#include "network/slip/PassASlipDrains.hpp" // the PASS A response drain stages\n'
                              + inc, 1)
    write_work(ASM, new_asm)
    write_work("include/network/slip/PassASlipScan.hpp", scan_hpp)
    write_work("src/core/network/slip/PassASlipScan.cpp", scan_cpp)
    write_work("include/network/slip/PassASlipDrains.hpp", drains_hpp)
    write_work("src/core/network/slip/PassASlipDrains.cpp", drains_cpp)
    o2, hpp, cpp = build_sd(sde, texts)
    new_sde = splice(SDE, 731, 1312, o2, sde.L)
    new_sde = new_sde.replace('#include "network/ar/SdEwlAr.hpp"',
                              '#include "network/ar/SdEwlAr.hpp"\n#include "network/ar/SdEwlRows.hpp"', 1)
    write_work(SDE, new_sde)
    write_work("include/network/ar/SdEwlRows.hpp", hpp)
    write_work("src/core/network/ar/SdEwlRows.cpp", cpp)
    cm = read_work("CMakeLists.txt")
    a = "\tsrc/core/network/slip/GfArcJump.cpp\n"
    b = "    src/core/network/ar/SdEwlAr.cpp\n"
    assert a in cm and b in cm
    cm = cm.replace(a, a + "\tsrc/core/network/slip/PassASlipScan.cpp\n\tsrc/core/network/slip/PassASlipDrains.cpp\n", 1)
    cm = cm.replace(b, b + "    src/core/network/ar/SdEwlRows.cpp\n", 1)
    write_work("CMakeLists.txt", cm)
    print("written")


def main(write):
    asm = Cut(ASM, VIEW_ORDER, VIEW_BIND, ALIASES)
    texts = {"scan_defs": [func_text(asm, s) for s in SCAN], "drain_defs": []}
    for s in DRAINS:
        t = func_text(asm, s)
        if s.get("ret_name"):
            t = t[:-1].rstrip() + f"\n    return {s['ret_name']};\n}}"
        texts["drain_defs"].append(t)
    sde = Cut(SDE, SD_ORDER, SD_BIND, {})
    sd_defs = []
    for s in SD_STAGES:
        body = rename_code("\n".join(sde.body(s["parts"]))).split("\n")
        head = []
        for a, b in s.get("head", []):
            head += rename_code("\n".join(sde.body([("L", a, b)]))).split("\n") + [""]
        names = used_names("\n".join(body + head))
        binds = [f"    {SD_BIND[n]} = v.{n};" for n in SD_ORDER if n in names]
        pre = binds + ([""] if binds else []) + head
        if s.get("forwarder"):
            pre += ["    " + ln for ln in s["forwarder"].split("\n")] + [""]
        txt = signature(s.get("ret", "void"), s["name"], s["params"], " {").split(chr(10)) + pre + body
        while txt[-1].strip() == "":
            txt.pop()
        txt.append("}")
        sd_defs.append("\n".join(txt))
    texts["sd_defs"] = sd_defs
    for k in ("scan_defs", "drain_defs", "sd_defs"):
        print(k, [d.count("\n") + 1 for d in texts[k]])
    if write:
        emit_all(asm, sde, texts)


if __name__ == "__main__":
    main("--write" in sys.argv)
