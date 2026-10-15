"""The canonical rows of card NOM3 step PB1a (design scripts/nom3_designs/PB1_design.md, 3.2).

A row names no line: `start` is the first text of the frozen piece (a banner comment or a
statement head) inside Application::networkPassB_ at the base, and a piece runs from its start
to the line before the next row's start (trailing blank lines trimmed).  ORDER is the frozen
order of every piece of PASS B after its alias block; a piece whose `name` is None stays in
networkPassB_ (the orchestrator), every other piece becomes the named stage.
"""

BASE = "7141240"
ASM = "src/core/app/ApplicationNetworkAssembly.cpp"

# The pieces of PASS B after its alias block, in frozen order.
ORDER = [
    dict(piece="S02", name=None, start="// --- PASS B: per-station assembly, then serial scatter"),
    dict(piece="S04", name="networkShDevCarryAge", start="// Age the SH-deviation carries once per epoch"),
    dict(piece="S05", name="networkCouplingReinitHistory", start="// Coupling-row time update (Card 1d"),
    dict(piece="S06", name=None, start="#pragma omp parallel for schedule(dynamic)"),
    dict(piece="S07", name="networkAssemblyTally", start="// Coupling observability (Card 1/1d), serial and post-produce"),
    dict(piece="S08", name="networkPrefitFuseEpoch", start="// --- Structural fuse (docs/SPEC_prefit_census_observability.md"),
    dict(piece="S09", name="networkAssemblyConsume", start="// --- Name the null column behind each addStation PD rejection"),
    dict(piece="S12", name="networkSolveStationsTrack", start="// --- In-solve count and invariant #7's re-coupling edge"),
    dict(piece="S13", name="networkEq34PinAudit", start="// Chain-pin audit (SPEC_synthetic_minimum_network"),
    dict(piece="S14", name=None, start="// Close the assembly wall-clock timer (D0"),
]

# The stages.  params: the parameters after (NetworkRun& run, const NetworkEpoch& ep), kept
# values first, then outputs.  guard: the frozen `if (C) {` that encloses the whole piece
# (GUARD: `if (!C) return;` and the body one level out).  ret_stmt: RETURN, appended.
# call: the orchestrator's call line (STAGE_CALL), at the frozen position of the piece.
STAGES = {
    "networkShDevCarryAge": dict(
        home="NetworkShDevVtec", ret="void", params=[], guard="sh_dev_on",
        call="networkShDevCarryAge(run, nep);",
        brief=("Ages the SH-deviation carry once per epoch, before PASS B's parallel produce "
               "reads it (T5v2; Iono-2 v3 de-mean; v3k erase-or-re-tap split)."),
        detail=("Every (station, satellite) in this epoch's slip funnel is erased (a "
                "raw-confirmed slip) or re-tapped to steady state (a D5-only entry under "
                "network.sh_dev_carry_d5_keep); every entry is then GM-aged to this epoch and "
                "dropped past 5 tau; the de-mean removes the information-weighted common mode. "
                "No-op unless NetworkKnobs::sh_dev_on.  Serial, called by networkPassB_ "
                "(card NOM3 step PB1a).")),
    "networkCouplingReinitHistory": dict(
        home="NetworkCoupling", ret="void", params=[], guard=None,
        call="networkCouplingReinitHistory(run, nep);",
        brief=("Records this epoch's reinit-history entries the coupling rows marginalize on "
               "(cards 1d and 1g)."),
        detail=("The inflate-q stash flags and the clock refloats (under 1d or 1g), and under "
                "1g the anchor handovers, eq34 re-references and datum-slip re-realizations. "
                "Serial, before PASS B's parallel produce reads the history; called by "
                "networkPassB_ (card NOM3 step PB1a).")),
    "networkAssemblyTally": dict(
        home="NetworkStationBuild", ret="void", params=["const EpochAssembly& assembly"],
        guard=None,
        call="networkAssemblyTally(run, nep, assembly);",
        brief=("Folds the per-station tallies of the produce into the run counters, serially "
               "and in station order."),
        detail=("The free-epoch skips, the parameter inventory drain, the response table, the "
                "oracle pin, the inject-side stale skips, the F3 block telemetry and its "
                "non-SPD warning, the LGM edges, the coupling-term census with the resume seam, "
                "card CTU and the SD-EWL trace.  Step PB2 splits it by family.  Called by "
                "networkPassB_ after the parallel produce (card NOM3 step PB1a)."),
        param_doc={"assembly": ("The station blocks networkPassB_ is assembling, read (a "
                                "group the caller still writes, so it is passed rather than "
                                "read through @p ep).")}),
    "networkPrefitFuseEpoch": dict(
        home="NetworkPrefitCensus", ret="void", params=["EpochAssembly& assembly"], guard=None,
        call="networkPrefitFuseEpoch(run, nep, assembly);",
        brief=("The structural fuse over every assembled station "
               "(SPEC_prefit_census_observability section 4): the last writer of each data "
               "row's weight."),
        detail=("No-op unless network.prefit_fuse_sigma > 0.  Called by networkPassB_ after "
                "the tally and before the consume (card NOM3 step PB1a)."),
        param_doc={"assembly": "[in/out] The station blocks; writes R.w."}),
    "networkAssemblyConsume": dict(
        home="NetworkSolveStations", ret="bool",
        params=["EpochPassA& pass_a", "EpochAssembly& assembly"], guard=None,
        ret_stmt="return epoch_ok;",
        call="const bool epoch_ok = networkAssemblyConsume(run, nep, pass_a, assembly);",
        brief=("Folds every assembled station into the estimator in station order."),
        detail=("The empty, mismatch and failed dispositions, addStation, the carried "
                "diagonal prior, the PD-rejection naming (card RF-PD) and its per-epoch "
                "roll-up.  Called by networkPassB_ (card NOM3 step PB1a)."),
        param_doc={"pass_a": "[in/out] present: a dropped station is cleared.",
                   "assembly": "[in/out] est_index_of, total_pins."},
        return_doc=("false when the legacy pivot's block was refused (the epoch is then "
                    "skipped by networkPassB_).")),
    "networkSolveStationsTrack": dict(
        home="NetworkSolveStations", ret="void",
        params=["const EpochAssembly& assembly", "bool epoch_ok"], guard="epoch_ok",
        call="networkSolveStationsTrack(run, nep, assembly, epoch_ok);",
        brief=("Counts the stations that entered the solve and marks every return "
               "(invariant 7's re-coupling edge, card RF-PD-fix)."),
        detail=("No-op on an aborted epoch.  Called by networkPassB_ after the consume (card "
                "NOM3 step PB1a)."),
        param_doc={"assembly": ("The station blocks, read (est_index_of, asm_res), passed "
                                "because networkPassB_ still writes the group."),
                   "epoch_ok": "The consume's verdict (kept: an orchestrator value)."}),
    "networkEq34PinAudit": dict(
        home="NetworkChainDatum", ret="void",
        params=["const EpochAssembly& assembly", "bool epoch_ok"],
        guard="phase_datum_eq34 && epoch_ok",
        call="networkEq34PinAudit(run, nep, assembly, epoch_ok);",
        brief=("The eq34 chain-pin audit: declared, folded, landed and short pins per group, "
               "and the seed-round targets of accepted stations for the next election."),
        detail=("No-op unless NetworkKnobs::phase_datum_eq34 and the epoch was not aborted. "
                "Called by networkPassB_ after the consume (card NOM3 step PB1a)."),
        param_doc={"assembly": ("The station blocks, read (pin_audit, pin_seed_targets, "
                                "est_index_of), passed because networkPassB_ still writes the "
                                "group."),
                   "epoch_ok": "The consume's verdict (kept)."}),
}

# The comment lines above each call group in networkPassB_ (STAGE_CALL; comments only).
CALL_GROUP_COMMENT = {
    "S04": "// The serial stages before the parallel produce (card NOM3 step PB1a).",
    "S07": "// The serial stages after the parallel produce (card NOM3 step PB1a).",
}

# KNOB_MOVE (row K1): the derived alias lines of networkPassB_ read their NetworkKnobs field.
# (frozen initializer text, new initializer text, knob field).
KNOB_MOVE = [
    ("pl_blk_on", "pl_local_block && carry_cpl_cond", "ctx.knobs.pl_blk_on"),
    ("zwd_blk_on", "ctx.knobs.zwd_in_block && pl_blk_on", "ctx.knobs.zwd_blk_on"),
    ("iono_blk_on", "ctx.knobs.iono_blk && pl_blk_on", "ctx.knobs.iono_blk_on"),
    ("rxclk_blk_on", "ctx.knobs.rxclk_blk && pl_blk_on", "ctx.knobs.rxclk_blk_on"),
    ("rxcb_blk_on", "ctx.knobs.rxcb_blk && pl_blk_on && rx_cb_on", "ctx.knobs.rxcb_blk_on"),
    ("relevel_free_on", "config_.networkCycleSlipRelevelFreeEpoch()", "ctx.knobs.relevel_free_on"),
]

# The harvest's four derivations (decision 6): file, alias name, the knob field it now reads.
HARVEST = "src/core/app/NetworkLocalCarryHarvest.cpp"
HARVEST_MOVE = ["zwd_blk_on", "iono_blk_on", "rxclk_blk_on", "rxcb_blk_on"]

KNOBS_CPP = "src/core/app/NetworkKnobs.cpp"
KNOBS_HPP = "include/app/NetworkRunContext.hpp"

NEW_FILES = ["NetworkStationBuild", "NetworkSolveStations"]
