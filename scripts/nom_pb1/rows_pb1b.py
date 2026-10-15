"""The canonical rows of card NOM3 step PB1b (design scripts/nom3_designs/PB1_design.md, 2.4-3.4).

A row names no line.  PIECES are anchors inside the try body of PASS B's parallel produce loop
in Application::networkPassB_ at BASE: an anchor is the first text of the piece's first line
(stripped), or, when it starts with a space, the whole raw line; each resolves to the first
matching line after the previous anchor, and a piece runs to the line before the next anchor.
TOP and CARRY are the two step tables in walk order (kStationBuildSteps, kStationCarrySteps);
each row names its frozen pieces and the declared transforms that apply to it
(scripts/nom_pb1/rows_pb1b.py is read by scripts/nom_pb1b_cut.py and scripts/nom_pb1b_verify.py).

Transforms (design 3.3): GUARD (`guard`: the piece is one `if (C) {...}`; it becomes
`if (!C) return ...;` with the body one level out), SPLIT_GUARD (`split`: the conjunction of
the frozen enclosing conditions the piece sits in, tested at its first line; `head_old`: a
frozen `if (X) {` line the split guard stands for; `tail_insert`: a closing brace inserted at
the end, for a frozen `if` whose close lies in a later piece), UNWRAP (`unwrap`), EXIT and
RESULT (data_rows), FRAME (FRAME_MEMBERS), CALLABLE (CALLABLES), TYPE_MOVE (PlPend),
MEMBER_TO_FREE (HELPERS), PURE_DECL_MOVE (the frame predicate and do_amb_carry), and the
declared output moves (OUTPUT_MOVES).
"""

BASE = "8f4f1db"
ASM = "src/core/app/ApplicationNetworkAssembly.cpp"

PIECES = [
    ("prod_head", "NetStation& st = stations[i];"),
    ("T1", "EpochMeasurement em = st.mm->computeMeasurementUpdate("),
    ("T2", "// Card L5B: the re-coupling edge of an estimated receiver IFB"),
    ("T3", "// --- Station congruence screen"),
    ("T4", "// The anchor station(s) fold tight-prior pins"),
    ("T5", "// GLONASS local-nuisance anchors"),
    ("T6", "// Test-only zero-ionosphere rows"),
    ("T7", "// Ionosphere constraint (W6)"),
    ("T8", "// --- SD pre-reduce EWL constraint"),
    ("LLI", "// --- Phase-leveling carry: record present ambiguities"),
    ("T9", "// --- RAIM pool re-host"),
    ("T10", "// The gate admits rx_cb_on, rx_cb_datum and opin_on alongside"),
    ("c1", "if (legacy_local_inject) {"),
    ("c2", "// --- I-in-block (network.carry_iono_in_block)"),
    ("c3", "// --- RX-CLK-in-block (network.carry_rx_clock_in_block)"),
    ("c4", "// RXCB-IN-BLOCK (card CTU section 14)"),
    ("c5", "// Card LGM (network.phase_colour_state, section 4.1)"),
    ("c6", "// Off: iono_bc and rxclk_bc are always empty"),
    ("drop_dac", "}  // do_amb_carry"),
    ("c7", "// --- Simulation-only oracle ambiguity pin"),
    ("c8", "// --- ZWD carry (B9)"),
    ("drop_lli", "}   // legacy_local_inject"),
    ("c9", "// --- rx-CB carry (docs/SPEC_rx_cb_carry.md)"),
    ("c10", "if (carry_cpl_cond) {"),
    ("c11", "                {"),
    ("T10close", "            }"),
    ("T11", "// --- Card LGM: the phase-colour columns' record and birth rows"),
    ("T12", "// --- Unconditional Z-row capture"),
    ("prod_tail", "R.ok = true;"),
]
# Frozen lines dropped: the closing braces of the two frozen blocks the carry steps split.
DROPPED_PIECES = ("drop_dac", "drop_lli")

STATION_KINDS_FIRST, STATION_KINDS_LAST = "DataCode", "PriorRxCbDatumPrePd"

TOP = [
    dict(name="data_rows", func="networkStationDataRows", home="NetworkStationSplit",
         pieces=["T1"], kinds=["DataCode", "DataCodeCompanion", "DataPhase", "DataPhaseSecond"],
         brief="The station's measurement update and its data rows (the split).",
         records="h2local, n_data_rows, param_inv; Done on empty innovations or an H/state "
                 "mismatch (empty_innov, state_mismatch)."),
    dict(name="rx_ifb_recouple", func="networkRxIfbRecoupleEdge", home="NetworkRxCodeBiasCarry",
         pieces=["T2"], kinds=[], guard="ctx.param_inventory.rx_ifb.estimated_by_knob",
         brief="Card L5B: the re-coupling edge of an estimated receiver IFB (invariant 7).",
         records="st.l5b.recouple, st.l5b.last_est_epoch."),
    dict(name="congruence_geometry", func="networkCongruenceRowGeometry", home="NetworkCongruence",
         pieces=["T3"], kinds=[], guard="congruence_rows && em.H.rows() > 0 && st.ecef.norm() > 1.0",
         brief="The station congruence screen's per-row position partials (CG-INC-0).",
         records="row_pos_enu."),
    dict(name="phase_datum_pins", func="networkPhaseDatumPinRows", home="NetworkPhaseDatum",
         pieces=["T4"], kinds=["AmbiguityPin", "Eq34RpbPin", "Eq34RpbRankFloor", "Eq34RpbTimeLink"],
         brief="The phase-datum pins of the station's anchored ambiguities and the eq34 receiver "
               "phase-bias rows.",
         records="np, pin_audit, pin_seed_targets, pl_rpb, prm_rpb_gap_skip."),
    dict(name="glonass_anchors", func="networkGlonassAnchorRows", home="NetworkStationAnchors",
         pieces=["T5"], kinds=["GloIfbAnchor", "GloIonoAnchor"],
         guard="config_.enableGlonassNetwork()",
         brief="GLONASS local-nuisance anchors (code-only network scope).",
         records="the anchor rows."),
    dict(name="sim_zero_iono", func="networkSimZeroIonoRows", home="NetworkStationAnchors",
         pieces=["T6"], kinds=["SimZeroIono"],
         guard="config_.getNetworkConfig().sim_zero_iono_sigma_m > 0.0",
         brief="Test-only zero-ionosphere rows (card ARL, network.sim_zero_iono_sigma_m).",
         records="the rows."),
    dict(name="iono_constraint", func="networkIonoConstraintRows", home="NetworkIonoConstraintRows",
         pieces=["T7"], kinds=["ShIonoTie", "GimIono"],
         brief="The ionosphere constraint (W6): the SH-VTEC tie rows or the external GIM rows.",
         records="gim_rows."),
    dict(name="sd_ewl", func="networkSdEwlStationRows", home="NetworkSdEwlAr",
         pieces=["T8"], kinds=["SdEwl"], guard="config_.getNetworkConfig().sd_ewl.enable",
         brief="The SD pre-reduce EWL constraint rows (docs/SPEC_sd_ewl_prereduce.md).",
         records="sd_ewl_trace, sd_ewl_rows; st.sd_ewl_ctr."),
    dict(name="raim_columns", func="networkRaimColumnCensus", home="NetworkRaim",
         pieces=["T9"], kinds=[], guard="raim_ci_on || pd_handover_continuous",
         brief="The RAIM pool re-host's carry-neutral checkable-column census.",
         records="qc_amb."),
    dict(name="carried_priors", func="networkStationCarriedPriors", home="NetworkStationCarry",
         pieces=["LLI", "T10"], kinds="carry", guard="legacy_local_inject || rx_cb_on || rx_cb_datum || opin_on",
         brief="The carried-prior section: builds the station's StationCarryFrame and walks "
               "kStationCarrySteps over it.",
         records="the frame (its steps write the records)."),
    dict(name="phase_colour_rows", func="networkPhaseColourRows", home="NetworkPhaseColourCarry",
         pieces=["T11"], kinds=["PhaseColourBirth", "PhaseColourBackstop"], guard="ctx.knobs.pc_on",
         brief="Card LGM: the phase-colour columns' record and birth rows.",
         records="pl_pc, pc_birth_rows, pc_backstop_rows."),
    dict(name="zwd_capture", func="networkZwdRowCapture", home="NetworkZwdCarry",
         pieces=["T12"], kinds=[], guard="zwd_capture && !R.pl_zwd_present",
         brief="The unconditional Z-row capture of a snapshot-armed run (section 17 finding 3).",
         records="pl_zwd, pl_zwd_present."),
]

CARRY = [
    dict(name="ambiguity", func="networkAmbiguityCarryColumns", home="NetworkLevelingCarry",
         pieces=["c1"], kinds=["PriorAmbiguityCarry", "CouplingScalar"],
         split=["legacy_local_inject"], head_old="if (legacy_local_inject) {", tail_insert="}",
         brief="The leveling carry's ambiguity columns: the inline route (a diagonal prior or a "
               "coupling row) or, under the local block, the pending list.",
         records="pl_amb, relevel_free_skips, inject_stale_skips, prm_amb_*, amb_state_fold; "
                 "pl_pend."),
    dict(name="slant_iono_block", func="networkSlantIonoBlockColumns", home="NetworkSlantIonoCarry",
         pieces=["c2"], kinds=[], split=["legacy_local_inject", "do_amb_carry"],
         brief="I-in-block: the station's carried slant-ionosphere columns for the local block.",
         records="pl_iono, pl_blk_iono_gap_drop, prm_iono_gap_skip; iono_bc."),
    dict(name="rx_clock_block", func="networkRxClockBlockColumns", home="NetworkRxClockCarry",
         pieces=["c3"], kinds=[], split=["legacy_local_inject", "do_amb_carry"],
         brief="RX-CLK-in-block: the station's carried receiver-clock columns for the local block.",
         records="pl_rxclk and its gap counters; rxclk_bc, rxclk_pred."),
    dict(name="rx_cb_block", func="networkRxCbBlockColumns", home="NetworkRxCodeBiasCarry",
         pieces=["c4"], kinds=[], split=["legacy_local_inject", "do_amb_carry"],
         brief="RXCB-IN-BLOCK: the station's carried receiver code-bias columns for the local block.",
         records="pl_rxcb_restored_skip, prm_rxcb_gap_skip; rxcb_bc, rxcb_ifb."),
    dict(name="phase_colour_block", func="networkPhaseColourBlockColumns", home="NetworkPhaseColourCarry",
         pieces=["c5"], kinds=[], split=["legacy_local_inject", "do_amb_carry"],
         brief="Card LGM: the station's carried phase-colour columns for the local block.",
         records="pc_planned; pc_bc, pc_pred."),
    dict(name="local_block", func="networkStationLocalBlock", home="NetworkCoupling",
         pieces=["c6"], kinds=["BlockAmbiguityCarry", "BlockZwdCarry", "BlockIonoCarry",
                                "BlockRxClockCarry", "BlockRxCbCarry", "BlockPhaseColourCarry",
                                "CouplingScalar"],
         split=["legacy_local_inject", "do_amb_carry"],
         brief="F3: the station's local carry block over the collected columns, or its "
               "per-column fallback.",
         records="pl_blk_*, pl_zwd*, pl_cb_injected, pc_folded, pc_*_folds; st.l5b.blk_folds."),
    dict(name="oracle_pin", func="networkOraclePinPriors", home="NetworkOraclePin",
         pieces=["c7"], kinds=["PriorAmbiguityOraclePin"], split=["legacy_local_inject"],
         guard="opin_on",
         brief="The simulation-only oracle ambiguity pin's diagonal prior rows.",
         records="opin_*."),
    dict(name="zwd", func="networkZwdCarryPrior", home="NetworkZwdCarry",
         pieces=["c8"], kinds=["PriorZwdCarry", "CouplingScalar"], split=["legacy_local_inject"],
         guard="zwd_on || zwd_seam_live",
         brief="The ZWD carry (B9): a diagonal prior or a coupling row on the wet delay.",
         records="pl_zwd*, pl_blk_zwd_fallback_diag, prm_zwd_gap_skip."),
    dict(name="rx_cb", func="networkRxCbPriors", home="NetworkRxCodeBiasCarry",
         pieces=["c9"], kinds=["PriorRxCbDatum", "PriorRxCbDatumPrePd", "PriorRxCbCarry"],
         guard="rx_cb_on || rx_cb_datum",
         brief="The rx-CB carry's diagonal priors and the RXCB-DATUM rows.",
         records="pl_cb, pl_cb_*, prm_rxcb_*; st.l5b.diag_*."),
    dict(name="coupling_region", func="networkCouplingRegionRows", home="NetworkCoupling",
         pieces=["c10"], kinds=["CouplingScalar", "BlockAmbiguityCarry", "BlockZwdCarry",
                                 "BlockIonoCarry", "BlockRxClockCarry", "BlockRxCbCarry",
                                 "BlockPhaseColourCarry"],
         guard="carry_cpl_cond",
         brief="Appends the station's pending coupling rows and block rows (the coupling region).",
         records="cpl_row_start, the coupling census."),
    dict(name="prior_finalize", func="networkStationPriorFinalize", home="NetworkStationCarry",
         pieces=["c11"], kinds=[], unwrap=True,
         brief="Sorts and coalesces the diagonal priors into the station's slot.",
         records="carry_*, prior_*, carry_merged, prior_merged."),
]

# The frame (StationCarryFrame) members: name -> (the binding a step uses, the frozen type text).
FRAME_MEMBERS = [
    ("legacy_local_inject", "const bool legacy_local_inject = carry.legacy_local_inject;", "const bool"),
    ("do_amb_carry", "const bool do_amb_carry = carry.do_amb_carry;", "const bool"),
    ("x_lin", "const Eigen::VectorXd& x_lin = carry.x_lin;", "const Eigen::VectorXd"),
    ("cb", "CouplingRowBuilder& cb = carry.cb;", "CouplingRowBuilder"),
    ("inj", "std::vector<std::tuple<Eigen::Index, double, double, RowTag>>& inj = carry.inj;",
     "std::vector<std::tuple<Eigen::Index, double, double, RowTag>>"),
    ("inj_prior", "std::vector<std::tuple<Eigen::Index, double, double, RowTag>>& inj_prior = "
                  "carry.inj_prior;", "std::vector<std::tuple<Eigen::Index, double, double, RowTag>>"),
    ("pl_pend", "std::vector<PlPend>& pl_pend = carry.pl_pend;", "std::vector<PlPend>"),
    ("iono_bc", "std::vector<LocalBlockCol>& iono_bc = carry.iono_bc;", "std::vector<LocalBlockCol>"),
    ("rxclk_bc", "std::vector<LocalBlockCol>& rxclk_bc = carry.rxclk_bc;", "std::vector<LocalBlockCol>"),
    ("rxclk_pred", "std::vector<LocalCarry>& rxclk_pred = carry.rxclk_pred;", "std::vector<LocalCarry>"),
    ("rxcb_bc", "std::vector<LocalBlockCol>& rxcb_bc = carry.rxcb_bc;", "std::vector<LocalBlockCol>"),
    ("rxcb_ifb", "std::vector<char>& rxcb_ifb = carry.rxcb_ifb;", "std::vector<char>"),
    ("pc_bc", "std::vector<LocalBlockCol>& pc_bc = carry.pc_bc;", "std::vector<LocalBlockCol>"),
    ("pc_pred", "std::vector<LocalCarry>& pc_pred = carry.pc_pred;", "std::vector<LocalCarry>"),
]
# Frame members whose frozen declaration carries an initializer moved into the frame's
# construction (PURE_DECL_MOVE / FRAME): the construction holds the frozen initializer text.
FRAME_INIT = ("do_amb_carry", "x_lin", "cb")

# Root bindings (ROOT): the closed list, canonical order.  A carry step or helper first binds
# `StationBuild& build = carry.build;`.  The verifier checks each against the frozen root of
# the same name (declarator equal, `const` may be added).
ROOTS = [
    ("ctx", "const NetworkRunContext& ctx = build.ctx;"),
    ("config_", "const GnssConfig& config_ = build.config;"),
    ("est", "const NetworkEstimator& est = build.est;"),
    ("setup", "const NetworkSetupResult& setup = build.setup;"),
    ("se", "const SyncedEpoch& se = build.se;"),
    ("carry_warm", "bool carry_warm = build.clock.carry_warm;"),
    ("pl_now_s", "double pl_now_s = build.clock.pl_now_s;"),
    ("modeled", "const std::vector<std::vector<PppObservation>>& modeled = build.pass_a.modeled;"),
    ("shared_phase_keys",
     "const std::set<std::string>& shared_phase_keys = build.pass_a.shared_phase_keys;"),
    ("i", "const int i = build.i;"),
    ("st", "NetStation& st = build.st;"),
    ("R", "StationAssembly& R = build.R;"),
    ("h2local", "std::map<int, int>& h2local = R.h2local;"),
    ("em", "const EpochMeasurement& em = build.em;"),
]
ROOT_DEPS = {"h2local": ["R"]}
BUILD_FROM_CARRY = "StationBuild& build = carry.build;"

# CALLABLE: the three lambdas of the ambiguity carry (frozen in piece c1) as functions in
# NetworkLevelingCarry.cpp; a step that calls one binds the frozen name to a forwarder.
CALLABLES = [
    dict(lam="pl_finish_var", func="networkAmbCarryFinishVar", ret="double", const_frame=True),
    dict(lam="amb_state_note", func="networkAmbStateNote", ret="void", const_frame=False),
    dict(lam="pl_emit_row", func="networkAmbCarryEmitRow", ret="void", const_frame=False),
]
CALLABLE_HOME = "NetworkLevelingCarry"
TYPE_MOVE = "PlPend"

# MEMBER_TO_FREE: Application's five PASS B row builders as free functions.
HELPERS = [
    dict(old="networkIsGaugePinned_", new="networkIsGaugePinned", home="NetworkPhaseDatum", lead=[]),
    dict(old="networkSplitStation_", new="networkSplitStation", home="NetworkStationSplit",
         lead=["const GnssConfig& config_"]),
    dict(old="networkAppendSdEwlRows_", new="networkAppendSdEwlRows", home="NetworkSdEwlAr",
         lead=["const GnssConfig& config_"]),
    dict(old="networkAppendGimIonoRows_", new="networkAppendGimIonoRows",
         home="NetworkIonoConstraintRows",
         lead=["const GnssConfig& config_", "const std::unique_ptr<GlobalIonosphereMap>& gim_"]),
    dict(old="networkAppendShIonoRows_", new="networkAppendShIonoRows",
         home="NetworkIonoConstraintRows", lead=["const GnssConfig& config_"]),
]
# Call-site edits (MEMBER_TO_FREE): old call head -> new call head, in the step bodies.
CALL_SITES = [
    ("networkSplitStation_(st, ", "networkSplitStation(config_, st, "),
    ("networkAppendShIonoRows_(st, ", "networkAppendShIonoRows(config_, st, "),
    ("networkAppendGimIonoRows_(st, ", "networkAppendGimIonoRows(config_, build.gim, st, "),
    ("networkAppendSdEwlRows_(st, ", "networkAppendSdEwlRows(config_, st, "),
    ("networkIsGaugePinned_(stations, ", "networkIsGaugePinned(stations, "),
]
# EXIT and RESULT (data_rows).
EXIT = ("continue;", "return StationStep::Done;")
RESULT = ("EpochMeasurement em = st.mm->computeMeasurementUpdate(",
          "build.em = st.mm->computeMeasurementUpdate(",
          "EpochMeasurement& em = build.em;")

# The families HV1 completes with their harvest halves (decision 5).
HARVEST_FAMILIES = ("NetworkLevelingCarry", "NetworkPhaseColourCarry", "NetworkSlantIonoCarry",
                    "NetworkRxClockCarry", "NetworkRxCodeBiasCarry", "NetworkZwdCarry")

NEW_FAMILIES = {
    "NetworkStationSplit": "One station's data rows: the measurement update and the column split "
                           "into the shared border and the local block.",
    "NetworkRxCodeBiasCarry": "The receiver code-bias carry family: the IFB re-coupling edge, the "
                              "in-block columns, the diagonal priors and the RXCB-DATUM rows.",
    "NetworkStationAnchors": "The station's anchor rows: GLONASS local-nuisance anchors and the "
                             "test-only zero-ionosphere rows.",
    "NetworkIonoConstraintRows": "The ionosphere constraint rows (W6): the SH-VTEC tie and the "
                                 "external GIM rows.",
    "NetworkStationCarry": "The station's carried-prior section: the frame, the walk of "
                           "kStationCarrySteps, and the diagonal prior finalize.",
    "NetworkPhaseColourCarry": "The phase-colour carry family (card LGM): the in-block columns "
                               "and the record and birth rows.",
    "NetworkZwdCarry": "The wet-delay carry family (B9): the diagonal or coupling prior and the "
                       "unconditional Z-row capture.",
    "NetworkLevelingCarry": "The leveling carry family: the ambiguity columns and the three "
                            "ambiguity-carry helpers.",
    "NetworkSlantIonoCarry": "The slant-ionosphere carry family (I-in-block): the in-block columns.",
    "NetworkRxClockCarry": "The receiver-clock carry family (RX-CLK-in-block): the in-block columns.",
}

# Declared output moves (design 3.4): (file, old text, new text[, count]): the old text occurs
# `count` times (default 1) and every occurrence moves; reverse.sed maps the new text back.
OUTPUT_MOVES = [
    ("src/core/config/GnssConfigLoadNetwork.cpp", "(networkAppendSdEwlRows_ returns ",
     "(networkAppendSdEwlRows returns "),
    ("src/core/config/GnssConfigLoadNetwork.cpp", "x_lin(cb)) (ApplicationNetworkAssembly.cpp), so",
     "x_lin(cb)) (NetworkRxCodeBiasCarry.cpp), so"),
    ("src/core/config/GnssConfigLoadNetwork.cpp", "ambiguity time-link and ApplicationNetworkAssembly's do_amb_carry ",
     "ambiguity time-link and NetworkStationCarry's do_amb_carry "),
    ("src/core/config/GnssConfigLoadNetwork.cpp", "(ApplicationNetworkAssembly's `do_amb_carry` requires",
     "(NetworkStationCarry's `do_amb_carry` requires"),
    ("src/core/filter/NetworkGrossLos.cpp", "\"networkSplitStation_ and keys on",
     "\"networkSplitStation and keys on"),
    ("src/core/filter/NetworkSignalIntegrity.cpp", "in networkSplitStation_ is compiled in",
     "in networkSplitStation is compiled in"),
    ("src/core/filter/NetworkMdb.cpp", "(ApplicationNetworkAssembly.cpp, `do_amb_carry`)",
     "(NetworkStationCarry.cpp, `do_amb_carry`)"),
    ("src/core/filter/NetworkParamRegistry.cpp", "(networkSplitStation_; network.station_marker_to_arp",
     "(networkSplitStation; network.station_marker_to_arp"),
    ("src/core/filter/NetworkResponses.cpp", "\"ApplicationNetworkAssembly.cpp PASS B inject\"",
     "\"NetworkLevelingCarry.cpp PASS B inject\"", 4),
    ("src/core/filter/NetworkResponses.cpp", "\"ApplicationNetworkAssembly.cpp PASS B in-block inject\"",
     "\"NetworkSlantIonoCarry.cpp PASS B in-block inject\""),
    ("src/core/filter/NetworkResponses.cpp", "\"ApplicationNetworkAssembly.cpp diagonal route innovation test\"",
     "\"NetworkRxCodeBiasCarry.cpp diagonal route innovation test\""),
    ("src/core/filter/NetworkResponses.cpp",
     "\"ApplicationNetworkAssembly.cpp PASS B inject gap gates (block and diagonal)\"",
     "\"NetworkRxCodeBiasCarry.cpp PASS B inject gap gates (block and diagonal)\""),
    ("src/core/filter/NetworkResponses.cpp", "\"ApplicationNetworkAssembly.cpp PASS B inject (block)\"",
     "\"NetworkRxClockCarry.cpp PASS B inject (block)\""),
    ("src/core/filter/NetworkResponses.cpp",
     "\"ApplicationNetworkAssembly.cpp the eq34 receiver phase-bias gap gate\"",
     "\"NetworkPhaseDatum.cpp the eq34 receiver phase-bias gap gate\""),
    ("src/core/filter/NetworkResponses.cpp", "\"ApplicationNetworkAssembly.cpp PASS B inject gap gates\"",
     "\"NetworkZwdCarry.cpp PASS B inject gap gates\""),
]
CONTRIB = "src/core/filter/NetworkContributions.cpp"
# Decision 3: the station kinds' builder column of "contributions as armed" from the step rows.
# kind -> (step name, the function that creates its rows, the file).  The old texts are the
# base's literals (read by the generator for reverse.sed).
CONTRIB_BUILDERS = {
    "kSplit": ("data_rows", "MeasurementModel::computeMeasurementUpdate, networkSplitStation",
               "NetworkStationSplit.cpp"),
    "kPinFold": ("phase_datum_pins", "networkPhaseDatumPinRows", "NetworkPhaseDatum.cpp"),
    "kGloFold": ("glonass_anchors", "networkGlonassAnchorRows", "NetworkStationAnchors.cpp"),
    "kLgmRows": ("phase_colour_rows", "networkPhaseColourRows", "NetworkPhaseColourCarry.cpp"),
}
# Kinds whose builder literal is replaced by a new per-kind constant (the old shared one stays
# for the other kinds where it applies).  kind -> (new constant, step, function, file)
CONTRIB_KIND_BUILDERS = {
    "SimZeroIono": ("kSimZeroIono", "sim_zero_iono", "networkSimZeroIonoRows", "NetworkStationAnchors.cpp"),
    "ShIonoTie": ("kShIonoRows", "iono_constraint", "networkAppendShIonoRows", "NetworkIonoConstraintRows.cpp"),
    "GimIono": ("kGimIonoRows", "iono_constraint", "networkAppendGimIonoRows", "NetworkIonoConstraintRows.cpp"),
    "SdEwl": ("kSdEwlRows", "sd_ewl", "networkAppendSdEwlRows", "NetworkSdEwlAr.cpp"),
    "PriorAmbiguityCarry": ("kAmbCarryRow", "ambiguity", "networkAmbCarryEmitRow", "NetworkLevelingCarry.cpp"),
    "PriorZwdCarry": ("kZwdCarryRow", "zwd", "networkZwdCarryPrior", "NetworkZwdCarry.cpp"),
    "PriorRxCbCarry": ("kRxCbRows", "rx_cb", "networkRxCbPriors", "NetworkRxCodeBiasCarry.cpp"),
    "PriorAmbiguityOraclePin": ("kOraclePinRows", "oracle_pin", "networkOraclePinPriors", "NetworkOraclePin.cpp"),
    "PriorRxCbDatum": ("kRxCbRows", "rx_cb", "networkRxCbPriors", "NetworkRxCodeBiasCarry.cpp"),
    "PriorRxCbDatumPrePd": ("kRxCbRows", "rx_cb", "networkRxCbPriors", "NetworkRxCodeBiasCarry.cpp"),
}


def builder_text(step, func, file):
    return f"{func} ({file}), station step {step}"
