"""Canonical table of card NOM3 step HV1: the local-carry harvest cut by carried kind.

Read by scripts/nom_hv1_cut.py (the generator) and scripts/nom_hv1_verify.py (gate G-R).

Coordinates are the design's P lines (scripts/nom3_designs/HV1_design.md, section 3.1: the
harvest as it read after the REMOVE-key deletion).  No line is trusted: each row's anchor (a
token prefix of one statement of the frozen function, matched at every statement start at
every depth, `at` moving it by whole statements at the same depth) is resolved on BASE, and
the row's pieces, drops, substitutions and extracts are moved by the difference between the
resolved line and the row's first code line.  The generator refuses a base on which an anchor
matches zero or several statements, or on which the moved row's first code line is not the
statement the anchor reached.

Row: (function, home, shape, pieces, extra)
  shape   orchestrator | guard (the first piece holds the `if (C) {` line, the last piece is
          its `}`: GUARD writes `if (!C) return;` and removes the `}`) | unwrap (a bare block:
          its `{` and `}` removed) | lambda | extracted | verbatim | drop
  pieces  [(a, b, shift)]  frozen ranges, comments included, `shift` leading spaces removed
  extra   anchor, at, first_code, subs {line: (old, new)}, drops {(a, b): kind},
          extract [(a, b, function, call)], merge {...}, kept (the signature after run, ep)
"""

BASE = "b7d4721"      # nom/wave2: main + NOM3 EP2, EP3, EP4, SG1-SG3, PB1a, PB1b
FROZEN = "src/core/app/NetworkLocalCarryHarvest.cpp"
FUNC_NAME = "networkHarvestLocalCarries"
P_FUNC = (45, 1915)
P_SIGNATURE = (45, 58)
P_ALIAS = (59, 143)

SPINE = "app/NetworkLocalCarryHarvest"
LEV = "filter/NetworkLevelingCarry"

BORDER = "HarvestBorder& border"
BLOCKS = "const std::vector<StationHarvestBlock>& blocks"
SB = "const StationHarvestBlock& sb"
I = "std::size_t i"
CALL_COL = ("harvestCoupling(", "networkHarvestColumn(run, ep, border, ")

ROWS = [
    ("networkHarvestLocalCarries", SPINE, "orchestrator",
     [(216, 222, 0), (380, 380, 0), (1028, 1031, 0), (1749, 1749, 0), (1915, 1915, 0)],
     {"anchor": "if (carry_warm) {", "kept": ()}),
    ("networkHarvestHandoverLedger", LEV, "guard",
     [(145, 166, 0), (167, 213, 4), (214, 214, 0)],
     {"anchor": "if (ctx.knobs.pd_handover_continuous) {", "kept": ()}),
    ("networkHarvestBorderBegin", SPINE, "verbatim", [(223, 276, 4)],
     {"anchor": "std::map<Eigen::Index, CoupledShared> col2desc;", "kept": (), "ret": "HarvestBorder",
      "subs": {227: ("std::map<Eigen::Index, CoupledShared> col2desc;",
                     "std::map<Eigen::Index, CoupledShared>& col2desc = border.col2desc;"),
               228: ("Eigen::MatrixXd cov_df;", "Eigen::MatrixXd& cov_df = border.cov_df;"),
               231: ("ClockCloneHarvest ccl_h;", "ClockCloneHarvest& ccl_h = border.ccl_h;"),
               276: ("SharedHistoryHarvest shx_h = ", "border.shx_h = ")}}),
    ("networkHarvestColumn", SPINE, "lambda", [(277, 283, 8), (284, 332, 8)],
     {"anchor": "NetworkNormalEquations::StationRegression reg;", "first_code": 288,
      "kept": (BORDER, "std::size_t est_idx", "Eigen::Index col",
               "const std::vector<Eigen::Index>* cross_cols", "Eigen::VectorXd* cross"),
      "ret": "std::pair<double, std::vector<CoupledShared>>",
      "drops": {(284, 287): "lambda_head", (332, 332): "lambda_close"}}),
    ("<hand-forward>", None, "drop", [],
     {"anchor": "std::vector<std::shared_ptr<PlHarvestBlock>> zblk_of_sta;", "first_code": 338,
      "drops": {(333, 379): "hand_forward"}}),
    ("networkHarvestStationBlock", SPINE, "verbatim", [(381, 495, 8)],
     {"anchor": "if (!phase_datum_prioritized && stations[i].is_pivot) continue;", "at": -1,
      "kept": (BORDER, I), "ret": "StationHarvestBlock",
      "subs": {381: ("continue;", "return sb;"), 385: ("continue;", "return sb;"),
               402: ("continue;", "return sb;"), 453: ("continue;", "return sb;")},
      "drops": {(468, 491): "hand_forward"}}),
    ("networkHarvestAmbiguityStation", LEV, "verbatim", [(496, 873, 8)],
     {"anchor": "for (size_t a = 0; a < R.pl_amb.size(); ++a) {", "kept": (BORDER, SB, I),
      "subs": {505: CALL_COL,
               # D11: a declared comment edit (the F3 admission age gate is retired)
               691: ("birth_ep (the clock of the F3 block admission age",
                     "birth_ep (the clock of the candidacy bar, and of the"),
               692: ("gate and of the candidacy bar) and its seconds twin",
                     "retired F3 block admission age gate) and its seconds twin")},
      "extract": [(518, 636, "networkAmbiguityMonitorFeeds",
                   "networkAmbiguityMonitorFeeds(run, ep, i, pa, value_abs, var);"),
                  (652, 678, "networkResumeCouplingCensusRow",
                   "networkResumeCouplingCensusRow(run, old->second, coupling);")]}),
    ("networkAmbiguityMonitorFeeds", LEV, "extracted", [(518, 636, 12)],
     {"anchor": "const bool prires_want = prires_pl && prires_est.enabled();",
      "kept": ("const std::size_t i", "const PlAmb& pa", "const double value_abs",
               "const double var")}),
    ("networkResumeCouplingCensusRow", "app/NetworkWarmResume", "extracted", [(652, 678, 20)],
     {"anchor": "const auto idOf = [](const CoupledShared& c) {", "run_only": True,
      "kept": ("const LocalCarry& restored", "const std::vector<CoupledShared>& coupling"),
      "subs": {665: ("old->second.coupling", "restored.coupling")},
      "merge": {"twin": (1612, 1638),
                "twin_anchor": "const auto idOfZ = [](const CoupledShared& c) {",
                "rename": {"idOfZ": "idOf", "znow": "now", "zmaxabs": "maxabs",
                           "zrhs_new": "rhs_new", "zrhs_old": "rhs_old",
                           "zdmax": "dmax", "zmatched": "matched",
                           "zgone": "gone", "zold": "old"}}}),
    ("networkAmbIfTraceStation", "filter/NetworkAmbStateTrace", "guard",
     [(874, 883, 8), (884, 985, 12), (986, 986, 8)],
     {"anchor": "if (ctx.knobs.amb_if_trace && hblk && !R.pl_amb.empty()) {", "kept": (SB, I)}),
    ("networkHarvestPhaseColourStation", "filter/NetworkPhaseColourCarry", "guard",
     [(987, 993, 8), (994, 1026, 12), (1027, 1027, 8)],
     {"anchor": "if (pc_in_cols && carry_cpl_cond) {", "kept": (BORDER, SB, I),
      "subs": {1001: CALL_COL}}),
    ("networkPrunePhaseColour", "filter/NetworkPhaseColourCarry", "verbatim", [(1033, 1043, 4)],
     {"anchor": "if (ctx.knobs.pc_on)", "kept": ()}),
    ("networkPruneAmbiguities", LEV, "unwrap", [(1045, 1052, 4), (1053, 1105, 8), (1106, 1106, 4)],
     {"anchor": "{ const long long fleet_before =", "kept": ()}),
    ("networkHarvestSlantIonoBlock", "filter/NetworkSlantIonoCarry", "guard",
     [(1107, 1128, 4), (1129, 1210, 8), (1211, 1211, 4)],
     {"anchor": "if (iono_blk_on) { auto& iono_blk_carry", "kept": (BORDER, BLOCKS),
      "subs": {1135: ("iblk_of_sta[i]", "blocks[i].segmentBlock(BlockSegment::Iono)"),
               1137: ("&iblk_cols_of_sta[i]", "&blocks[i].cols"),
               1138: ("iblk_row0_of_sta[i]", "blocks[i].rowOf(BlockSegment::Iono)"),
               1158: CALL_COL}}),
    ("networkHarvestRxClock", "filter/NetworkRxClockCarry", "guard",
     [(1212, 1233, 4), (1234, 1409, 8), (1410, 1410, 4)],
     {"anchor": "if (rxclk_blk_on) { auto& rx_clk_carry", "kept": (BORDER, BLOCKS),
      "subs": {1250: ("rblk_of_sta[i]", "blocks[i].segmentBlock(BlockSegment::RxClock)"),
               1252: ("&rblk_cols_of_sta[i]", "&blocks[i].cols"),
               1253: ("rblk_row0_of_sta[i]", "blocks[i].rowOf(BlockSegment::RxClock)"),
               1273: CALL_COL}}),
    ("networkHarvestRxCbBlock", "filter/NetworkRxCodeBiasCarry", "guard",
     [(1411, 1419, 4), (1420, 1511, 8), (1512, 1512, 4)],
     {"anchor": "if (rxcb_blk_on) { auto& rx_cb_carry", "kept": (BORDER, BLOCKS),
      "subs": {1426: ("cblk_of_sta[i]", "blocks[i].segmentBlock(BlockSegment::RxCodeBias)"),
               1428: ("&cblk_cols_of_sta[i]", "&blocks[i].cols"),
               1429: ("cblk_row0_of_sta[i]", "blocks[i].rowOf(BlockSegment::RxCodeBias)"),
               1438: CALL_COL}}),
    ("networkHarvestZwd", "filter/NetworkZwdCarry", "guard",
     [(1513, 1521, 4), (1522, 1697, 8), (1698, 1698, 4)],
     {"anchor": "if (zwd_on || zwd_capture || zwd_seam_live) {", "kept": (BORDER, BLOCKS),
      "subs": {1549: ("zblk_of_sta[i]", "blocks[i].segmentBlock(BlockSegment::Zwd)"),
               1551: ("&zblk_cols_of_sta[i]", "&blocks[i].cols"),
               1554: CALL_COL},
      "extract": [(1612, 1638, "networkResumeCouplingCensusRow",
                   "networkResumeCouplingCensusRow(run, zold->second, coupling);"),
                  (1670, 1697, "networkZwdSeamWindowStep", "networkZwdSeamWindowStep(run, ep);")]}),
    ("networkZwdSeamWindowStep", "filter/NetworkZwdCarry", "guard",
     [(1670, 1676, 8), (1677, 1696, 12), (1697, 1697, 8)],
     {"anchor": "if (zwd_seam_live) {", "kept": ()}),
    ("networkHarvestBorderEnd", SPINE, "verbatim", [(1699, 1705, 4)],
     {"anchor": "networkClockCloneHarvestEnd(ccl_h, ctx);", "kept": (BORDER,)}),
    ("networkHarvestRxPhaseBias", "filter/NetworkChainDatum", "guard",
     [(1706, 1712, 4), (1713, 1747, 8), (1748, 1748, 4)],
     {"anchor": "if (eq34_rpb_q > 0.0) {", "kept": ()}),
    ("networkRxCbDatumGaugeCensus", "filter/NetworkRxCodeBiasCarry", "guard",
     [(1751, 1763, 0), (1764, 1768, 4), (1769, 1769, 0)],
     {"anchor": "if (ctx.knobs.rx_cb_datum) {", "kept": ()}),
    ("networkHarvestRxCbCarry", "filter/NetworkRxCodeBiasCarry", "guard",
     [(1771, 1779, 0), (1780, 1893, 4), (1894, 1894, 0)],
     {"anchor": "if (ctx.knobs.rx_cb_on) {", "kept": ()}),
    ("networkResumeFirstEpochRetire", "app/NetworkWarmResume", "verbatim", [(1896, 1914, 0)],
     {"anchor": "if (ctx.carry.resume_cpl_census_pending)", "run_only": True, "kept": ()}),
]

# HV1b (FIELD_MOVE): the four rx-CB run fields move from ctx.carry to ctx.counters.
FIELD_MOVE = ("rx_cb_injected_total", "rx_cb_innov_skips_total", "rx_cb_harvested_total",
              "rx_cb_sta_tally")

# The orchestrator's calls, in order, and its declared insert lines (stripped text).
ORCH_INSERTS = [
    "networkHarvestHandoverLedger(run, ep);",
    "HarvestBorder border = networkHarvestBorderBegin(run, ep);",
    "std::vector<StationHarvestBlock> blocks(stations.size());",
    "blocks[i] = networkHarvestStationBlock(run, ep, border, i);",
    "if (!blocks[i].ok) continue;",
    "networkHarvestAmbiguityStation(run, ep, border, blocks[i], i);",
    "networkAmbIfTraceStation(run, ep, blocks[i], i);",
    "networkHarvestPhaseColourStation(run, ep, border, blocks[i], i);",
    "networkPrunePhaseColour(run, ep);",
    "networkPruneAmbiguities(run, ep);",
    "networkHarvestSlantIonoBlock(run, ep, border, blocks);",
    "networkHarvestRxClock(run, ep, border, blocks);",
    "networkHarvestRxCbBlock(run, ep, border, blocks);",
    "networkHarvestZwd(run, ep, border, blocks);",
    "networkHarvestBorderEnd(run, ep, border);",
    "networkHarvestRxPhaseBias(run, ep);",
    "networkRxCbDatumGaugeCensus(run, ep);",
    "networkHarvestRxCbCarry(run, ep);",
    "networkResumeFirstEpochRetire(run);",
]

# The frozen parameters and their bindings (networkLeveling's EP1 preamble, table 1.4).
# Declarator text is the frozen parameter's; `const` is added on est (decision D5).
PARAMS = [
    ("ctx", "NetworkRunContext& ctx", "run.ctx"),
    ("config_", "const GnssConfig& config_", "run.config"),
    ("est", "const NetworkEstimator& est", "run.engines.est"),
    ("x_s", "const Eigen::VectorXd& x_s", "ep.solve.x_s"),
    ("stations", "std::vector<NetStation>& stations", "run.setup.stations"),
    ("est_index_of", "const std::vector<int>& est_index_of", "ep.assembly.est_index_of"),
    ("asm_res", "const std::vector<StationAssembly>& asm_res", "ep.assembly.asm_res"),
    ("plan", "const NetworkSbasis::Plan& plan", "ep.solve.plan"),
    ("sh_model", "const SphericalHarmonicIonosphere* sh_model", "run.setup.sh_model.get()"),
    ("sh_model2", "const SphericalHarmonicIonosphere* sh_model2", "run.setup.sh_model2.get()"),
    ("carry_vce_est", "NetworkCarryVce& carry_vce_est", "run.engines.carry_vce"),
    ("prires_est", "NetworkPriorResidual& prires_est", "run.engines.prires"),
    ("carry_warm", "bool carry_warm", "ep.clock.carry_warm"),
    ("pl_now_s", "double pl_now_s", "ep.clock.pl_now_s"),
    ("registered_clock_sats", "const std::vector<std::string>& registered_clock_sats",
     "ep.columns.registered_clock_sats"),
    ("shared_phase_keys", "const std::set<std::string>& shared_phase_keys",
     "ep.pass_a.shared_phase_keys"),
    ("registered_code_keys",
     "const std::vector<std::pair<std::string, std::string>>& registered_code_keys",
     "ep.columns.registered_code_keys"),
]

# Harvest-local bindings: the border members (functions taking the border) and the station
# layout (functions taking sb).  RE_EVALUATED lines repeat the frozen loop-head text.
BORDER_BIND = {
    "col2desc": "std::map<Eigen::Index, CoupledShared>& col2desc = border.col2desc;",
    "cov_df": "Eigen::MatrixXd& cov_df = border.cov_df;",
    "ccl_h": "ClockCloneHarvest& ccl_h = border.ccl_h;",
    "shx_h": "SharedHistoryHarvest& shx_h = border.shx_h;",
}
LAYOUT_BIND = {
    "R": "const StationAssembly& R = asm_res[i];",
    "est_idx": "const std::size_t est_idx = static_cast<std::size_t>(est_index_of[i]);",
    "cols": "const std::vector<Eigen::Index>& cols = sb.cols;",
    "xr": "const Eigen::VectorXd& xr = sb.xr;",
    "vard": "const Eigen::VectorXd& vard = sb.vard;",
    "hblk": "const std::shared_ptr<PlHarvestBlock>& hblk = sb.blk;",
    "pc_row0": "const Eigen::Index pc_row0 = sb.rowOf(BlockSegment::PhaseColour);",
    "pc_in_cols": "const bool pc_in_cols = ctx.knobs.pc_on && !R.pl_pc.empty();",
}
RE_EVALUATED = ("R", "est_idx", "pc_in_cols")
LAYOUT_SEG = {"iblk": "Iono", "rblk": "RxClock", "cblk": "RxCodeBias", "zblk": "Zwd"}

# Drop-kind validators (design 3.1).
HF_NAMES = ("zblk_of_sta", "zblk_cols_of_sta",
            "iblk_of_sta", "iblk_cols_of_sta", "iblk_row0_of_sta",
            "rblk_of_sta", "rblk_cols_of_sta", "rblk_row0_of_sta",
            "cblk_of_sta", "cblk_cols_of_sta", "cblk_row0_of_sta")
HF_FLAGS = ("zwd_blk_on", "iono_blk_on", "rxclk_blk_on", "rxcb_blk_on",
            "zwd_in_cols", "iono_in_cols", "rxclk_in_cols", "rxcb_in_cols")

# HV1c: the response sites' printed "where" (kTable of filter/NetworkResponses.cpp).
CITATIONS = {
    "AmbGapPrune": "NetworkLevelingCarry.cpp", "IonoGapPrune": "NetworkSlantIonoCarry.cpp",
    "PcGapPrune": "NetworkPhaseColourCarry.cpp", "RxcbStepErase": "NetworkRxCodeBiasCarry.cpp",
    "RxcbGapPrune": "NetworkRxCodeBiasCarry.cpp", "RxclkJumpErase": "NetworkRxClockCarry.cpp",
    "RxclkGapPrune": "NetworkRxClockCarry.cpp", "RpbGapPrune": "NetworkChainDatum.cpp",
    "ZwdGapPrune": "NetworkZwdCarry.cpp",
}

# Each row's first code line (P), the statement its anchor must reach (design 3.1).
FIRST_CODE = {
    "networkHarvestLocalCarries": 222, "networkHarvestHandoverLedger": 166,
    "networkHarvestBorderBegin": 227, "networkHarvestColumn": 288, "<hand-forward>": 338,
    "networkHarvestStationBlock": 381, "networkHarvestAmbiguityStation": 496,
    "networkAmbiguityMonitorFeeds": 541, "networkResumeCouplingCensusRow": 652,
    "networkAmbIfTraceStation": 883, "networkHarvestPhaseColourStation": 993,
    "networkPrunePhaseColour": 1037, "networkPruneAmbiguities": 1052,
    "networkHarvestSlantIonoBlock": 1128, "networkHarvestRxClock": 1233,
    "networkHarvestRxCbBlock": 1419, "networkHarvestZwd": 1521,
    "networkZwdSeamWindowStep": 1676, "networkHarvestBorderEnd": 1701,
    "networkHarvestRxPhaseBias": 1712, "networkRxCbDatumGaugeCensus": 1763,
    "networkHarvestRxCbCarry": 1779, "networkResumeFirstEpochRetire": 1903,
}
