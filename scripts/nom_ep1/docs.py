"""The comment rewrites of card NOM3 step EP1 (the review round of 2026-10-14).

Step EP1 moves code; the comments around it that described the old shape (the per-epoch
lambda, its subBegin / subEnd closures, the solve's callbacks, the driver's epoch locals, the
engines as run-TU locals) would otherwise describe code that is gone.  Each rewrite below is
a declared hunk the generator applies to its finished text, after every transform, so a
regeneration from the base reproduces it and nothing is edited by hand:

  DocHunk(path, old, new, why, when, regex, count)
    old / new  literal text (regex: a regular expression and its replacement) of the file
               as the generator writes it; a literal must occur exactly `count` times (one by
               default), a regular expression at least once (or exactly `count` when given).
    when       "" applies at every stage; a seam's name applies only when that seam is
               converted (the hunk rewrites text the conversion writes, a converted
               declaration's Doxygen).
    after_deletion  the text is the base's after the REMOVE-key deletion (which rewrote
               these comments with the code it took); at a base that still has every object
               it takes (the EP0 base, model.ABSENT empty) the hunk is listed, not applied.

Every hunk changes comments only: the generator lexes the file before and after it (comments
dropped, nom_cpp.lex) and refuses a hunk that changes a token, so the relocation verifier,
which ignores comments throughout, sees none of them.  A hunk whose text is not found is a
generator problem (exit 1 unless --allow-problems), never applied elsewhere.

Where a sentence records history (what the code was before EP1), the rewrite annotates it
("until card NOM3 step EP1") rather than deleting it.  A comment block whose only subject left
runNetwork with the lambda goes, and what it said that is still true moves to the
declaration it describes.

STALE lists the spellings of the old shape; the generator reports every remaining hit in the
tree it writes (src/, include/, tests/) as the stale-spelling census, a worklist for the docs
pass of the last merge.  A remaining hit is not a failure: some record history on purpose
("formerly the driver closure subBegin").
"""
import re
from dataclasses import dataclass

DRIVER = "src/core/app/ApplicationNetwork.cpp"
EPOCH = "src/core/app/ApplicationNetworkEpoch.cpp"
RUN_CPP = "src/core/app/NetworkRun.cpp"
APP_HPP = "include/app/Application.hpp"
RUNCTX = "include/app/NetworkRunContext.hpp"
SOLVE_HPP = "include/filter/NetworkSolveGlue.hpp"
DDB_HPP = "include/filter/NetworkDdPriorBuild.hpp"
WLTYPES = "include/filter/NetworkWlRunTypes.hpp"
GROUPS = "include/filter/NetworkRunGroups.hpp"
PRIRES = "include/filter/NetworkPriorResidual.hpp"
CENSUS = "include/filter/NetworkRedundancyCensus.hpp"


@dataclass(frozen=True)
class DocHunk:
    path: str
    old: str
    new: str
    why: str
    when: str = ""
    regex: bool = False
    count: int = 0      # literal: 0 means exactly one; regex: 0 means at least one
    after_deletion: bool = False


# The seam headers whose banner or Doxygen says "the caller keeps the subBegin/subEnd(SB_X)
# brackets" (the frozen closures, now networkSubBegin / networkSubEnd over the run).
BRACKET_HEADERS = (
    "include/filter/NetworkClockQc.hpp", "include/filter/NetworkDdFrac.hpp",
    "include/filter/NetworkEmitStore.hpp", "include/filter/NetworkInstallCarry.hpp",
    "include/filter/NetworkIonoDiag.hpp", "include/filter/NetworkLeveling.hpp",
    "include/filter/NetworkProducerAr.hpp", "include/filter/NetworkRaim.hpp",
    "include/filter/NetworkShDevVtec.hpp", "include/filter/NetworkShHealth.hpp",
    "include/filter/NetworkSolPrep.hpp", "include/filter/NetworkStashStatus.hpp",
    APP_HPP,
)

DOC_HUNKS = (
    # --- runNetwork (ApplicationNetwork.cpp) ---------------------------------------------
    DocHunk(DRIVER,
            "    // The monitor's hourly trace flush is scheduled by ctx.carry.first_vce_s and\n"
            "    // ctx.carry.last_vce_hour, which networkShHealth writes.\n"
            "    // Apply step (M2 v2): arms the LASER 5-95 variance-factor iteration over the\n"
            "    // monitor above.  Unlike the monitor's estimator this is not a run-TU local:\n"
            "    // α² is cross-epoch state read at three fold sites in three translation units\n",
            "    // The carry LS-VCE monitor is an engine the run owns (run.engines.carry_vce; a\n"
            "    // run-TU local until card NOM3 step EP1).  Its hourly trace flush is scheduled by\n"
            "    // ctx.carry.first_vce_s and ctx.carry.last_vce_hour, which networkShHealth writes.\n"
            "    // Apply step (M2 v2): arms the LASER 5-95 variance-factor iteration over that\n"
            "    // monitor.  Unlike the monitor's estimator this is not engine state:\n"
            "    // α² is cross-epoch state read at three fold sites in three translation units\n",
            "the VCE monitor this comment followed moved into the run's engines"),
    DocHunk(DRIVER,
            "    // Prior-residual census.  A run-TU local for the same reason carry_vce_est is\n"
            "    // one (see the \"engine objects stay run-TU locals\" note in\n"
            "    // app/NetworkRunContext.hpp), threaded into the same four seams: SB_T1A\n",
            "    // Prior-residual census.  An engine the run owns, like the carry-VCE monitor\n"
            "    // (run.engines.prires and run.engines.carry_vce, app/NetworkRun.hpp; both were\n"
            "    // run-TU locals until card NOM3 step EP1), read by the same four seams: SB_T1A\n",
            "the census is a run-owned engine since EP1"),
    DocHunk(DRIVER,
            "    // The block-Baarda accumulator lives in ctx.carry (its feed site is inside\n"
            "    // the redundancy-census seam, which takes ctx but no run-TU reference), so it\n"
            "    // is seated here from config rather than constructed by the context.",
            "    // The block-Baarda accumulator lives in ctx.carry (its feed site is inside\n"
            "    // the redundancy-census seam, which took ctx but no run-TU reference until card\n"
            "    // NOM3 step EP1; the seam takes the run now, and moving the accumulator to the\n"
            "    // run's engines is a later step), so it is seated here from config rather than\n"
            "    // constructed by the context.",
            "the census seam takes NetworkRun& since EP1", when="networkRedundancyCensus"),
    DocHunk(DRIVER,
            "\n    // The gauge-pin predicate (\"is (sat, signal) at station i a gauge-pinned\n"
            "    // ambiguity this epoch?\") is networkIsGaugePinned_ in\n"
            "    // ApplicationNetworkAssembly.cpp, the single test that replaces the scattered\n"
            "    // is_pivot gauge tests.\n"
            "    //   Legacy (network.phase_datum_prioritized off): membership means the station\n"
            "    //     is the pivot, which pins the whole shared_phase_keys set, so the\n"
            "    //     predicate returns is_pivot and every rewritten site stays byte-identical.\n"
            "    //   Prioritized: membership means the \"sat_signal\" key is in station i's\n"
            "    //     elected pinned_amb_targets, the anchor for that gauge freedom, whose pin\n"
            "    //     is folded into the station's own local block.  The key form matches the\n"
            "    //     pin fold and the ambiguity-state key.\n"
            "    // The Eq-34 / de Jonge single-receiver-chain datum drives all the prioritized\n"
            "    // machinery (no pivot-first, every station carries its arcs, per-(sat,signal)\n"
            "    // shared set), diverging only at the election (a spanning-tree chain election) and\n"
            "    // the pin fold (per-tree-edge ambiguity pins with a target, plus a datum-receiver\n"
            "    // RPB pin).\n"
            "    // DD cycle-basis invariant carry gate, declared early so the superseded eq34\n"
            "    // hygiene knobs below can gate on it.  Requires phase_datum_eq34.\n",
            "\n",
            "the gauge-pin predicate's note followed the phase-datum aliases, which went with the "
            "lambda (its content moves to networkIsGaugePinned_'s Doxygen), and the carry gate's "
            "note its alias carry_invariant", after_deletion=True),
    DocHunk(DRIVER,
            "    // PhaseBias / local / RPB carries, the eq34 pin-round/shift hygiene for phase\n"
            "    // keys (bypassed below), and the carry S-transform hygiene: the datum is\n",
            "    // PhaseBias / local / RPB carries, the eq34 pin-round/shift hygiene for phase\n"
            "    // keys (bypassed in the epoch function), and the carry S-transform hygiene: the datum is\n",
            "the bypass is in the epoch function since EP1"),
    DocHunk(DRIVER,
            "    // Estimation-side fine-slip innovation gate: the invariant-carry-native\n"
            "    // replacement for the RAIM fine-slip layer that the invariant bypass empties.\n"
            "    // Resolved in NetworkKnobs::snapshot; off, the output is byte-identical.\n",
            "",
            "the note of the alias carry_innov_gate, which went with the lambda (the knob's own "
            "documentation in NetworkKnobs says the same)"),
    DocHunk(DRIVER,
            "    // The assembly helpers are Application member functions in\n"
            "    // ApplicationNetworkAssembly.cpp: networkIsGaugePinned_, networkSplitStation_,\n"
            "    // networkAppendGimIonoRows_ and networkAppendShIonoRows_ (formerly the lambdas\n"
            "    // isGaugePinned, splitStation, appendGimIonoRows and appendShIonoRows).  Their former\n"
            "    // [&] captures are explicit parameters or member reads.  The SH-deviation carry's\n"
            "    // switches (sh_dev_tau, sh_dev_on, sh_dev_demean, sh_dev_d5_keep) are ctx.knobs fields,\n"
            "    // read by networkPassB_ and networkShDevVtec.\n"
            "\n"
            "    // appendShIonoRows is Application::networkAppendShIonoRows_ in\n"
            "    // ApplicationNetworkAssembly.cpp; sh_model, shc and the sh_dev state\n"
            "    // are passed explicitly at the PASS-B call site.\n"
            "\n",
            "\n",
            "signposts of the INC-7 helpers' former lambdas and of the PASS-B call site, both in "
            "the epoch function since EP1 (the members' Doxygen in Application.hpp records INC-7)",
            after_deletion=True),

    # --- the epoch function (ApplicationNetworkEpoch.cpp) -----------------------------------
    DocHunk(EPOCH,
            "// The per-epoch body as a callable over `se` alone; every other name it reads is\n"
            "// loop-invariant setup or ctx-carried run state, captured by reference.  Loop\n"
            "// control maps one to one (a top-level `continue` becomes return Next, the\n"
            "// max-duration `break` becomes return Stop), so the batch iteration below is\n"
            "// structurally identical to an inline loop.  The callable exists so a streaming\n"
            "// epoch source (network.streaming, the RTF-3 multi-station front-end) can drive the\n"
            "// identical per-epoch body without a pre-built synced vector.\n"
            "// Card NOM3 step EP1: the callable is Application::networkProcessEpoch_, over the run\n"
            "// (app/NetworkRun.hpp) and the synced epoch.\n",
            "// The per-epoch body, a member function over the run (app/NetworkRun.hpp), which holds\n"
            "// the loop-invariant setup and the ctx-carried run state, and the synced epoch.  Until\n"
            "// card NOM3 step EP1 it was runNetwork's lambda processSyncedEpoch, a callable over `se`\n"
            "// that captured every other name by reference.  Loop control maps one to one (a\n"
            "// top-level `continue` became return Next, the max-duration `break` return Stop), so\n"
            "// runNetwork's batch iteration is structurally identical to an inline loop.  The\n"
            "// function exists so a streaming epoch source (network.streaming, the RTF-3\n"
            "// multi-station front-end) can drive the identical per-epoch body without a pre-built\n"
            "// synced vector; both epoch sources are in runNetwork (ApplicationNetwork.cpp).\n",
            "the lambda's banner described the callable and the iteration below it"),
    DocHunk(EPOCH,
            "    // (SkipEpoch return), and the Next return below bypasses subEnd(SB_PREASM).\n",
            "    // (SkipEpoch return), and the Next return below bypasses networkSubEnd(SB_PREASM).\n",
            "the bracket closure is networkSubEnd"),
    DocHunk(EPOCH,
            "    networkSubBegin(run, SB_GAUGEBUILD); // carry-driver lambda, pre-DD-build (D0b)\n",
            "    // The block ahead of the DD build (D0b); until card NOM3 step EP1 it also set up the\n"
            "    // carry-driver closure buildCarryDdPriors (the epoch function and the solve's replay\n"
            "    // now call networkBuildCarryDdPriors directly).\n"
            "    networkSubBegin(run, SB_GAUGEBUILD);\n",
            "the closure went (CALLBACK_DIRECT)", when="networkSolveGlue", after_deletion=True),
    DocHunk(EPOCH,
            "networkSubEnd(run, SB_GAUGEBUILD);   // close gauge build and carry-driver setup (D0b)\n"
            "    networkSubBegin(run, SB_DDBUILD);    // carry_driver.beginEpoch + buildCarryDdPriors, W·Pᶻ·Wᵀ (D0b)\n",
            "networkSubEnd(run, SB_GAUGEBUILD);   // close the block ahead of the DD build (D0b)\n"
            "    networkSubBegin(run, SB_DDBUILD);    // carry_driver.beginEpoch + networkBuildCarryDdPriors, W·Pᶻ·Wᵀ (D0b)\n",
            "the closure went (CALLBACK_DIRECT)", when="networkSolveGlue"),
    DocHunk(EPOCH,
            "networkSubEnd(run, SB_DDBUILD);      // close buildCarryDdPriors, the card-4 DD-cov projection (D0b)\n",
            "networkSubEnd(run, SB_DDBUILD);      // close networkBuildCarryDdPriors, the card-4 DD-cov projection (D0b)\n",
            "the closure went (CALLBACK_DIRECT)", when="networkSolveGlue"),

    # --- NetworkRun.cpp (the D0b note moved with the bracket closures) ----------------------
    DocHunk(RUN_CPP,
            "// With the toggle off, the subBegin/subEnd lambdas return early (no clock reads, no\n",
            "// With the toggle off, networkSubBegin / networkSubEnd (runNetwork's subBegin / subEnd\n"
            "// closures until card NOM3 step EP1) return early (no clock reads, no\n",
            "the closures are the two functions below"),

    # --- Application.hpp ----------------------------------------------------------------------
    DocHunk(APP_HPP,
            "     * the elected @c pinned_amb_targets entries.  Implemented in\n"
            "     * @c ApplicationNetworkAssembly.cpp.\n"
            "     */\n"
            "    bool networkIsGaugePinned_(",
            "     * the elected @c pinned_amb_targets entries.  Implemented in\n"
            "     * @c ApplicationNetworkAssembly.cpp.\n"
            "     *\n"
            "     * The single test that replaced the scattered is_pivot gauge tests: under the\n"
            "     * legacy datum it returns is_pivot, so every rewritten site stayed byte-identical;\n"
            "     * under the prioritized datum the \"sat_signal\" key form matches the pin fold and\n"
            "     * the ambiguity-state key, and the pin is folded into the station's own local\n"
            "     * block.  The Eq-34 / de Jonge single-receiver-chain datum drives the same\n"
            "     * prioritized machinery (no pivot-first, every station carries its arcs,\n"
            "     * per-(sat,signal) shared set) and diverges only at the election (a spanning-tree\n"
            "     * chain election) and the pin fold (per-tree-edge ambiguity pins with a target,\n"
            "     * plus a datum-receiver RPB pin).  (Moved here from runNetwork at card NOM3 step\n"
            "     * EP1, where it had stayed beside the phase-datum aliases.)\n"
            "     */\n"
            "    bool networkIsGaugePinned_(",
            "keeps what the runNetwork note said that this Doxygen did not"),
    DocHunk(APP_HPP,
            "     * without calling subEnd(SB_PREASM).  The epoch counts are the int&\n"
            "     * out-parameters.  Implemented in @c ApplicationNetworkAssembly.cpp.\n",
            "     * without calling networkSubEnd(SB_PREASM).  The epoch counts are members of\n"
            "     * @p columns (the int& out-parameters until card NOM3 step EP1).  Implemented in\n"
            "     * @c ApplicationNetworkAssembly.cpp.\n",
            "the counts are EpochColumns members", when="networkAssemblyRegisterColumns_"),
    DocHunk(APP_HPP,
            "     * prev_week/prev_tow advance).  @p asm_res, @p est_index_of and @p total_pins\n"
            "     * are the driver's epoch locals, filled here for the solve, RAIM and emit\n",
            "     * prev_week/prev_tow advance).  @p asm_res, @p est_index_of and @p total_pins\n"
            "     * are members of the epoch's assembly group (ep.assembly; the driver's epoch\n"
            "     * locals until card NOM3 step EP1), filled here for the solve, RAIM and emit\n",
            "the three are EpochAssembly members", when="networkPassB_"),

    # --- NetworkRunContext.hpp ----------------------------------------------------------------
    DocHunk(RUNCTX,
            " * (decomposition INC-4; see docs/network_decomposition_plan.md).  The run loop in\n"
            " * ApplicationNetwork.cpp binds `auto& X = ctx.carry.X;` / `const T X = ctx.knobs.X;`\n"
            " * aliases under those names, so the use sites read as before.\n",
            " * (decomposition INC-4; see docs/network_decomposition_plan.md).  The epoch function\n"
            " * (ApplicationNetworkEpoch.cpp) and the preamble of each seam in its epoch form bind\n"
            " * `auto& X = ctx.carry.X;` / `const T X = ctx.knobs.X;` aliases under those names (the\n"
            " * run loop in ApplicationNetwork.cpp did until card NOM3 step EP1), so the use sites read\n"
            " * as before.\n",
            "the aliases are bound by the epoch function and the seam preambles since EP1"),
    DocHunk(RUNCTX,
            " * Kept out of this header on purpose (they stay locals of the run TU): the\n"
            " * engine objects (NetworkEstimator, NetworkInvariantCarryDriver, NetworkCarryVce,\n"
            " * NetworkEmitRebase), the D0/D0b section-timer\n"
            " * instrumentation, and the closed-loop consumer metrics.  The driver holds them in\n"
            " * the groups of filter/NetworkRunGroups.hpp (engines, emit engines, run\n"
            " * timing, closed-loop tally), which the seams receive by reference.\n",
            " * Kept out of this header on purpose: the engine objects (NetworkEstimator,\n"
            " * NetworkInvariantCarryDriver, NetworkCarryVce, NetworkPriorResidual,\n"
            " * NetworkEmitRebase), the D0/D0b section-timer instrumentation, and the closed-loop\n"
            " * consumer metrics.  The run owns them (app/NetworkRun.hpp, card NOM3 step EP1; they\n"
            " * were locals of the run TU before) in the groups of filter/NetworkRunGroups.hpp\n"
            " * (engines, emit engines, run timing, closed-loop tally); a per-epoch seam in its epoch\n"
            " * form receives the run, NetworkRun&.\n",
            "NetworkRun owns the engines since EP1 (NetworkRunGroups.hpp points here as the authority)",
            after_deletion=True),
    DocHunk(RUNCTX,
            "    class NetworkPriorResidual;   ///< filter/NetworkPriorResidual.hpp; a run-TU local, like NetworkCarryVce.\n",
            "    class NetworkPriorResidual;   ///< filter/NetworkPriorResidual.hpp; an engine the run owns (run.engines.prires), like NetworkCarryVce.\n",
            "a run-owned engine since EP1"),
    DocHunk(RUNCTX,
            "the subBegin / subEnd brackets, the per-epoch readout)",
            "the networkSubBegin / networkSubEnd brackets, the per-epoch readout)",
            "the bracket closures are functions since EP1"),
    DocHunk(RUNCTX,
            "        // network_carry_vce.csv by content hour, and the monitor it flushes (the run-TU local\n"
            "        // NetworkCarryVce) is not snapshotted either,",
            "        // network_carry_vce.csv by content hour, and the monitor it flushes (NetworkCarryVce,\n"
            "        // run.engines.carry_vce; a run-TU local until card NOM3 step EP1) is not snapshotted\n"
            "        // either,",
            "a run-owned engine since EP1"),
    DocHunk(RUNCTX,
            "        // census's shape) because its feed site is inside the redundancy-census seam,\n"
            "        // which takes ctx but not a run-TU reference.  Default-constructed with no\n",
            "        // census's shape) because its feed site is inside the redundancy-census seam,\n"
            "        // which took ctx but not a run-TU reference until card NOM3 step EP1 (the seam\n"
            "        // takes the run now, so the reason no longer holds; moving the accumulator into\n"
            "        // the run's engines is a later step).  Default-constructed with no\n",
            "the census seam takes NetworkRun& since EP1", when="networkRedundancyCensus"),
    DocHunk(RUNCTX,
            "        // Lives here rather than as a run-TU local for the block census's reason: its feed\n"
            "        // site is inside the redundancy-census seam, which takes ctx but not a run-TU\n"
            "        // reference.  Default-constructed unarmed,",
            "        // Lives here rather than as a run-TU local for the block census's reason: its feed\n"
            "        // site is inside the redundancy-census seam, which took ctx but not a run-TU\n"
            "        // reference until card NOM3 step EP1 (a later step moves it with the block\n"
            "        // census).  Default-constructed unarmed,",
            "the census seam takes NetworkRun& since EP1", when="networkRedundancyCensus"),
    DocHunk(RUNCTX,
            "        // the loop-timing cluster; t_loop_begin stays a run-TU local).\n",
            "        // the loop-timing cluster; t_loop_begin stayed a run-TU local, and is\n"
            "        // run.timing.loop_begin since card NOM3 step EP1).\n",
            "the loop start is a run-owned timing member since EP1"),

    # --- the solve, the DD build, the census, the run types -----------------------------------
    DocHunk(SOLVE_HPP,
            " *     `LoopSignal::Next` before subEnd(SB_SOLVEGLUE), which keeps the original\n"
            " *     `continue`'s bypass of subEnd.\n",
            " *     `LoopSignal::Next` before networkSubEnd(SB_SOLVEGLUE), which keeps the original\n"
            " *     `continue`'s bypass of the bracket.\n",
            "the bracket closure is networkSubEnd"),
    DocHunk(SOLVE_HPP,
            " *     the caller re-aliases each with `auto& x = sg_.x;`, so downstream code is\n"
            " *     unchanged.\n",
            " *     the caller re-aliased each with `auto& x = sg_.x;` until card NOM3 step EP1;\n"
            " *     the epoch now stores the result as ep.solve and each seam binds the member it\n"
            " *     reads (`ep.solve.x`) in its preamble, so downstream code is unchanged.\n",
            "the result is ep.solve since EP1"),
    DocHunk(SOLVE_HPP,
            " * The within-epoch re-solve hands its culprit sets to buildCarryDdPriors through the\n"
            " * epoch workspace (@ref EpochWorkspace::we_culprits, card WCF): the T2 block adapts the\n"
            " * sets in that member, the driver's rebuild lambda reads the member of the same\n"
            " * workspace, and the block clears it when it ends.\n",
            " * The within-epoch re-solve hands its culprit sets to networkBuildCarryDdPriors through\n"
            " * the epoch workspace (@ref EpochWorkspace::we_culprits, card WCF): the T2 block adapts\n"
            " * the sets in that member, the replay's direct rebuild call reads the member of the same\n"
            " * workspace (the driver's rebuild lambda did until card NOM3 step EP1), and the block\n"
            " * clears it when it ends.\n",
            "the rebuild is a direct call since EP1 (CALLBACK_DIRECT)", when="networkSolveGlue"),
    DocHunk(DDB_HPP,
            " * The run TU keeps the lambda as a thin `[&]` wrapper that forwards to this free\n"
            " * function, so the initial DD build and the within-epoch RAIM re-solve\n"
            " * (`resolve_dropping`) keep byte-identical capture semantics.",
            " * Until card NOM3 step EP1 the run TU kept the lambda as a thin `[&]` wrapper that\n"
            " * forwarded to this free function, so the initial DD build and the within-epoch RAIM\n"
            " * re-solve (`resolve_dropping`) kept byte-identical capture semantics; the epoch\n"
            " * function and the solve's replay now call it directly over the run and the epoch.",
            "the wrapper went (CALLBACK_DIRECT)", when="networkSolveGlue", after_deletion=True),
    DocHunk(CENSUS,
            "schedule instead of adding their own. Pass @c sg_.x_s (the final accepted solution,\n"
            " *     after any within-epoch re-solve); an empty vector disarms both riders.\n",
            "schedule instead of adding their own. Pass @c sg_.x_s (the final accepted solution,\n"
            " *     after any within-epoch re-solve); an empty vector disarms both riders.  (sg_ was\n"
            " *     the driver's solve result until card NOM3 step EP1; the binding above reads it as\n"
            " *     ep.solve.x_s.)\n",
            "the frozen @param text stays whole (check V7); the annotation follows it",
            when="networkRedundancyCensus"),
    DocHunk(PRIRES,
            " * One instance per producer run, with the same lifetime and ownership as\n"
            " * @ref NetworkCarryVce: a run-TU local, passed by reference into the feed seams\n"
            " * (see the note on engine objects in the @c app/NetworkRunContext.hpp banner).\n",
            " * One instance per producer run, with the same lifetime and ownership as\n"
            " * @ref NetworkCarryVce: an engine the run owns (run.engines.prires, app/NetworkRun.hpp;\n"
            " * a run-TU local passed by reference until card NOM3 step EP1; see the note on engine\n"
            " * objects in the @c app/NetworkRunContext.hpp banner).\n",
            "a run-owned engine since EP1"),
    DocHunk(WLTYPES,
            "/// brackets the driver opens and closes with subBegin / subEnd, which tile the loop body\n",
            "/// brackets the epoch function opens and closes with networkSubBegin / networkSubEnd\n"
            "/// (runNetwork's subBegin / subEnd closures until card NOM3 step EP1), which tile the loop body\n",
            "the bracket closures are functions since EP1"),
    DocHunk(WLTYPES,
            "    SB_GAUGEBUILD,   //          the carry-driver lambda set-up ahead of the DD build\n"
            "    SB_DDBUILD,      //          buildCarryDdPriors: eq34 arc graph + carry_driver.buildDdPriors\n",
            "    SB_GAUGEBUILD,   //          the block ahead of the DD build (until card NOM3 step EP1,\n"
            "                     //          also the carry-driver closure's set-up)\n"
            "    SB_DDBUILD,      //          networkBuildCarryDdPriors: eq34 arc graph + carry_driver.buildDdPriors\n",
            "the closure went (CALLBACK_DIRECT)", when="networkSolveGlue", after_deletion=True),
    DocHunk(GROUPS,
            "/// member (the loop start, the D0b brackets' subEnd, the per-epoch readout), except the\n",
            "/// member (the loop start, the D0b brackets' networkSubEnd, the per-epoch readout), except the\n",
            "the bracket closure is networkSubEnd"),

    # --- the seam headers' bracket sentences --------------------------------------------------
    *(DocHunk(p, r"(?<![\w/])subBegin/subEnd\(", "networkSubBegin/End(",
              "the caller's brackets are networkSubBegin / networkSubEnd since EP1", regex=True)
      for p in BRACKET_HEADERS),
)

# The spellings of the shape EP1 removed (the census of the docs pass).
STALE = (
    ("subBegin / subEnd closures", r"(?<![\w])sub(Begin|End)\b"),
    ("sg_ (the solve result's old local)", r"\bsg_\b"),
    ("the carry-driver lambda", r"carry-driver lambda"),
    ("the solve's callbacks by their closure names", r"(?<![\w])build(CarryDdPriors|EnsembleGauges)\b"),
    ("an engine as a run-TU local", r"run-TU local(?!s? names)"),
    ("the per-epoch lambda", r"\bprocessSyncedEpoch\b|per-epoch lambda"),
)


def _code(text):
    from nom_cpp import lex
    return [(t.kind, t.text) for t in lex(text)]


def apply(outputs, read, converted, problems, deletion_landed=True, skipped=None):
    """Applies DOC_HUNKS to @p outputs ({path: text}, the generator's finished text), reading a
    file the generator did not otherwise write through @p read (the base text); each hunk
    whose `when` seam is not in @p converted is skipped, and each after_deletion hunk is
    listed in @p skipped instead of applied when @p deletion_landed is false.  Returns
    {path: number of hunks}.  A hunk not found is appended to @p problems; a hunk that
    changes code raises ValueError."""
    done = {}
    for h in DOC_HUNKS:
        if h.when and h.when not in converted:
            continue
        if h.after_deletion and not deletion_landed:
            if skipped is not None:
                skipped.append(f"{h.path}: {h.why}")
            continue
        text = outputs.get(h.path)
        if text is None:
            text = read(h.path)
        if text is None:
            problems.append(f"{h.path}: doc hunk ({h.why}): the file does not exist")
            continue
        if h.regex:
            new, n = re.subn(h.old, h.new, text)
            ok = n >= 1 if not h.count else n == h.count
        else:
            n = text.count(h.old)
            ok = n == (h.count or 1)
            new = text.replace(h.old, h.new) if ok else text
        if not ok:
            problems.append(f"{h.path}: doc hunk ({h.why}): its text occurs {n} times; left unchanged")
            continue
        if h.path.endswith((".cpp", ".hpp", ".h")) and _code(new) != _code(text):
            raise ValueError(f"{h.path}: doc hunk ({h.why}) changes code, not only comments")
        outputs[h.path] = new
        done[h.path] = done.get(h.path, 0) + 1
    return done


def census(texts):
    """[(label, path, line, text)] of every STALE spelling left in @p texts ({path: text})."""
    out = []
    pats = [(label, re.compile(p)) for label, p in STALE]
    for path in sorted(texts):
        text = texts[path]
        if text is None or not path.endswith((".cpp", ".hpp", ".h")):
            continue
        for k, line in enumerate(text.split("\n"), 1):
            for label, rx in pats:
                if rx.search(line):
                    out.append((label, path, k, line.strip()))
    return out
