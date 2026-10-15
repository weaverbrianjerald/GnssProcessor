"""The declared substitutions of card NOM3 step PB1a (design PB1, section 3.3), a closed list.

scripts/nom_pb1_verify.py accepts an edit of the frozen text only when it is listed here; each
entry carries its reason.  The generator's rows (scripts/nom_pb1/rows.py) hold the same facts
in the form the generator needs; the verifier reads only this file and the trees.
"""

BASE = "7141240"
ASM = "src/core/app/ApplicationNetworkAssembly.cpp"

# Each stage: its home (filter/<home>.hpp / .cpp), its parameter list after (run, ep), the
# frozen piece's first line, GUARD's condition and RETURN's statement, and the orchestrator's
# call line (STAGE_CALL), in frozen order.
STAGES = [
    ("networkShDevCarryAge", "NetworkShDevVtec", [],
     "// Age the SH-deviation carries once per epoch", "sh_dev_on", None,
     "networkShDevCarryAge(run, nep);"),
    ("networkCouplingReinitHistory", "NetworkCoupling", [],
     "// Coupling-row time update (Card 1d", None, None,
     "networkCouplingReinitHistory(run, nep);"),
    ("networkAssemblyTally", "NetworkStationBuild", ["const EpochAssembly& assembly"],
     "// Coupling observability (Card 1/1d), serial and post-produce", None, None,
     "networkAssemblyTally(run, nep, assembly);"),
    ("networkPrefitFuseEpoch", "NetworkPrefitCensus", ["EpochAssembly& assembly"],
     "// --- Structural fuse (docs/SPEC_prefit_census_observability.md", None, None,
     "networkPrefitFuseEpoch(run, nep, assembly);"),
    ("networkAssemblyConsume", "NetworkSolveStations", ["EpochPassA& pass_a", "EpochAssembly& assembly"],
     "// --- Name the null column behind each addStation PD rejection", None, "return epoch_ok;",
     "const bool epoch_ok = networkAssemblyConsume(run, nep, pass_a, assembly);"),
    ("networkSolveStationsTrack", "NetworkSolveStations", ["const EpochAssembly& assembly", "bool epoch_ok"],
     "// --- In-solve count and invariant #7's re-coupling edge", "epoch_ok", None,
     "networkSolveStationsTrack(run, nep, assembly, epoch_ok);"),
    ("networkEq34PinAudit", "NetworkChainDatum", ["const EpochAssembly& assembly", "bool epoch_ok"],
     "// Chain-pin audit (SPEC_synthetic_minimum_network", "phase_datum_eq34 && epoch_ok", None,
     "networkEq34PinAudit(run, nep, assembly, epoch_ok);"),
]
# The pieces that stay in networkPassB_ (first line of each), in frozen order with the stages.
KEPT = {
    "S02": "// --- PASS B: per-station assembly, then serial scatter",
    "S06": "#pragma omp parallel for schedule(dynamic)",
    "S14": "// Close the assembly wall-clock timer (D0",
}
# The comment lines STAGE_CALL adds above a call group (comments only; reason: a reader of
# networkPassB_ sees where the stages are).
CALL_COMMENTS = [
    "// The serial stages before the parallel produce (card NOM3 step PB1a).",
    "// The serial stages after the parallel produce (card NOM3 step PB1a).",
]

# ROOT realizations in a free function of names networkPassB_ bound otherwise: the frozen alias
# `auto log = log_manager_.getLogger("main_log");` is the run's logger (EP1's realization,
# NetworkRun::log is "main_log"); Application::config_ is the run's configuration.
FREE_ROOTS = {
    "log": "const std::shared_ptr<spdlog::logger>& log = run.log;",
    "config_": "const GnssConfig& config_ = run.config;",
}

# KNOB_MOVE: (alias, frozen initializer, new initializer, the snapshot statement).
KNOB_MOVE = [
    ("pl_blk_on", "pl_local_block && carry_cpl_cond", "ctx.knobs.pl_blk_on",
     "k.pl_blk_on = k.pl_local_block && k.carry_cpl_cond;"),
    ("zwd_blk_on", "ctx.knobs.zwd_in_block && pl_blk_on", "ctx.knobs.zwd_blk_on",
     "k.zwd_blk_on = k.zwd_in_block && k.pl_blk_on;"),
    ("iono_blk_on", "ctx.knobs.iono_blk && pl_blk_on", "ctx.knobs.iono_blk_on",
     "k.iono_blk_on = k.iono_blk && k.pl_blk_on;"),
    ("rxclk_blk_on", "ctx.knobs.rxclk_blk && pl_blk_on", "ctx.knobs.rxclk_blk_on",
     "k.rxclk_blk_on = k.rxclk_blk && k.pl_blk_on;"),
    ("rxcb_blk_on", "ctx.knobs.rxcb_blk && pl_blk_on && rx_cb_on", "ctx.knobs.rxcb_blk_on",
     "k.rxcb_blk_on = k.rxcb_blk && k.pl_blk_on && k.rx_cb_on;"),
    ("relevel_free_on", "config_.networkCycleSlipRelevelFreeEpoch()", "ctx.knobs.relevel_free_on",
     "k.relevel_free_on = config_.networkCycleSlipRelevelFreeEpoch();"),
]
KNOBS_CPP = "src/core/app/NetworkKnobs.cpp"
KNOBS_HPP = "include/app/NetworkRunContext.hpp"
# Decision 6: the harvest's four derivations read the knobs (same conjunct set).
HARVEST = "src/core/app/NetworkLocalCarryHarvest.cpp"
HARVEST_MOVE = ["zwd_blk_on", "iono_blk_on", "rxclk_blk_on", "rxcb_blk_on"]

# Value bindings whose source cannot change inside PASS B (no ALIAS_SPLIT staleness hazard):
# the knobs and the parameter registry are const after the snapshot, the configuration is
# never written during the run, and these epoch groups are not PASS B outputs.
STABLE_SOURCES = ("ctx.knobs.", "ctx.params.", "config_.", "ep.clock.", "ep.columns.", "ep.se",
                  "ep.funnel.", "setup.n_sta_total", "setup.parallel_on", "setup.n_threads")

# The aliases no function reads after the cut (dropped everywhere; reason: unread in the frozen
# function too).
DEAD = ["pl_on", "eq34_rpb_prior_sigma"]

# Literals that leave with a declared realization, not printed: networkPassB_ no longer reads the
# alias `auto log = log_manager_.getLogger("main_log");` (only the tally and the consume did, and
# they bind run.log, the same logger), so its lookup key leaves the file.
LITERAL_DROPS = ['"main_log"']

CONTRACT_TEST = "tests/test_network_epoch_contract.cpp"
