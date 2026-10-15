# NOM3 step EP2: pre-registration (the inline blocks move to their concerns)

Card: `docs/refound/NOM3_network_giants_2026-10-09.md` section 3, EP2. Base: main `7141240` (EP1
landed). Frozen text: `git show 7141240:src/core/app/ApplicationNetworkEpoch.cpp` (1 611 lines;
`Application::networkProcessEpoch_` is lines 154 to 1609). Branch `nom/ep2`. Pure relocation:
output byte-identical (gate G-EP, 23 whole-run arms, run by the orchestrator).

## 1. The block table (frozen lines -> function, file)

Every range is moved as text (indentation unchanged: depth 1 in the driver, depth 1 in the new
body). The comment lines that explain a call's position stay at the call site.

| # | frozen lines | what | new function (form) | file |
| --- | --- | --- | --- | --- |
| A1 | 213-217 | start-offset fast-forward | `networkFastForward(run, se_raw)` -> `EpochAdmission` | `app/NetworkEpochAdmission.{hpp,cpp}` (new) |
| A2 | 273-334 | one-epoch clears (attrib arm, trip, datum graph), broadcast feeds, max-duration, decimation, processed count | `networkAdmitEpoch(run, ep)` -> `EpochAdmission` {Process, Skip, Stop} | same |
| T1 | 345-352 | D0 baseline | `networkSectionTimersBegin(run)` -> `EpochSectionTimers` (run form) | `app/NetworkRun.{hpp,cpp}` |
| F1 | 396-417 | funnel opening: deferred innovation-gate flags, retap-only drain | `networkSlipFunnelOpen(run, ep, funnel)` | `filter/NetworkSlipFunnel.{hpp,cpp}` (new) |
| F2 | 449 | raw subset folded into the funnel | `networkSlipFunnelUnionRaw(run, ep, funnel)` | same |
| P1 | 510-1012 | shared phase-bias set and the anchor election (legacy, eq34, two-edge handover) | `networkPhaseAnchorEpoch(run, ep, pass_a)` | `filter/NetworkPhaseAnchorEpoch.{hpp,cpp}` (new) |
| P2 | 1024-1041 | eq34 receiver phase-bias allocation (D3c) | `networkReceiverPhaseBiasAllocation(run, ep)` | same |
| S1 | 1094-1106 | float-vehicle bounded drop | `networkAmbFloatDropEpoch(run, ep)` | `filter/NetworkAmbFloat.{hpp,cpp}` |
| G1 | 1166-1256 | gap-resume bridge census | `networkGapResumeCensus(run, ep)` (observer) | `slip/NetworkGapResumeCensus.{hpp,cpp}` (new) |
| S2 | 1260-1269 | DD-carry z-store predict | `networkCarryDriverBeginEpoch(run, ep)` | `filter/NetworkDdPriorBuild.{hpp,cpp}` |
| S3 | 1447 | block-Baarda predict half | `networkBlockBaardaPredict(run, ep)` | `filter/NetworkBlockBaarda.{hpp,cpp}` |
| S4 | 1451 | congruence predict half | `networkCongruencePredict(run, ep)` | `filter/NetworkCongruence.{hpp,cpp}` |
| S5 | 1542-1554 | LGM and AIL buffered-row drains | `networkBufferedTraceDrains(run, ep)` | `filter/NetworkAmbStateTrace.{hpp,cpp}` |
| T2 | 1566-1602 | D0 / D0b readout | `networkSectionTimersReadout(run, ep, sec)` | `app/NetworkRun.{hpp,cpp}` |

The admission is one verdict type at two sites, a declared deviation from the card's "one
function": the fast-forward must precede the event injector (a fast-forwarded epoch cannot
consume an event) and the signal roster, while the max-duration and decimation gates must follow
the roster (it advances `ctx.arc_ledger` on every synced epoch, the stopping one included). A
single function would have to own the injector, the screen, the epoch's construction and the
roster.

## 2. Driver-side gates moved to the seam's first line

The frozen `if (G) networkX(...);` becomes `networkX(...);`; the seam's first statement after
its bindings becomes `if (!(G)) return;` with G spelled through the seam's own bindings.

| frozen line | gate G | seam |
| --- | --- | --- |
| 1299 | `carry_invariant` (`ctx.knobs.carry_invariant`) | `networkInstallCarry` |
| 1314 | `ctx.knobs.gl_emit_exclude`, Decide phase only | `networkGrossLosScreen` (`phase == GrossLosPhase::Decide && !...`) |
| 1452 | `ctx.knobs.trace_postfit_on` | `networkPostfitSigma0` |
| 1454 | `ctx.knobs.trace_sh_spectrum` | `networkShSpectrum` |
| 1461 | `ctx.knobs.trace_station_block` | `networkStationBlock` |
| 1477 | `ctx.knobs.redun_census_on` | `networkRedundancyCensus` |
| 1485 | `ctx.knobs.bb_on` | `networkBlockBaardaEmit` |
| 1494 | `ctx.knobs.cg_on` | `networkCongruenceEmit` |
| 1504 | `ctx.knobs.mdb_on` | `networkMdbFlush` |
| 1084 | `ctx.carry.oracle_pin` | `networkOraclePinEpoch`: already its first two lines (`pin` null -> return); the driver gate is dropped |

Each of these seams has no caller but the driver (grep at the base), so the move changes no other
path. Gates left in the driver, because the seam's file belongs to a concurrent step: 1362
`networkPostFitRaim` (NetworkRaim, step SG), 1469 `networkPrefitCensus`, 1514
`networkDatumGraphCensus`, 1516 `networkRhsAttribution` (step SG files). Left on purpose: 1285
`CarryEigenScope` around the primary DD build (the solve glue's replay calls the same seam without
it; moving it in would thread the replay).

## 3. Declared substitutions and drops (closed list)

Anything else the verifier finds is a failure.

- **D1 return verdicts.** In A1/A2, `return LoopSignal::Next;` -> `return EpochAdmission::Skip;`,
  `return LoopSignal::Stop;` -> `return EpochAdmission::Stop;`; one added final
  `return EpochAdmission::Process;` per function. The driver maps them back (Stop -> Stop, Skip
  -> Next).
- **D2 bindings.** Each new function opens with the EP1 markers and one binding per frozen name
  its body uses: the frozen driver declarator with the run's root spelled out (`setup.X` ->
  `run.setup.X`, `ctx.X` stays through `NetworkRunContext& ctx = run.ctx;`, Application's
  `config_` -> `const GnssConfig& config_ = run.config;`), and an epoch local through its epoch
  member (`carry_slipped_ambs` -> `ep.funnel.carry_slipped_ambs`, `pl_now_s` ->
  `ep.clock.pl_now_s`, `modeled` / `present` -> `ep.pass_a.*`, `n_present` ->
  `ep.columns.n_present`, `sec_asm_ms_epoch` -> `ep.assembly.sec_asm_ms_epoch`).
- **D3 const tightening.** A name the moved body only reads may gain `const` (compile-proven: the
  body compiles against the const binding). A written name keeps the frozen category and binds
  through an output parameter, marked `// written`.
- **D4 the event forwarder.** The driver's `emitEvent` lambda (364-372) is dropped from the driver
  (no remaining reader) and re-declared, same text, in P1 and G1.
- **D5 gate moves.** Section 2, each row a frozen `if` line dropped from the driver plus one seam
  line added; the oracle-pin row drops the driver gate only.
- **D6 timer hand-off.** T1 returns `EpochSectionTimers{ sec_t0, sec_epoch_t0 }` (one added
  line); T2 binds `sec_t0` / `sec_epoch_t0` from it.
- **D7 dropped driver bindings.** A frozen driver binding (preamble 159-204, the epoch locals
  396, 424, 428, 430, 435, 347) that no remaining driver line reads is deleted.
- **D8 calls.** Each moved range is replaced in the driver by its one call (A2: by the call and
  the two verdict tests).

## 4. The verifier, `scripts/nom_ep2_verify.py` (reduced G-R, authorized)

Lexing and statements from `scripts/nom_cpp.py`. A statement line is a source line's code tokens
(comments dropped, whitespace normalized).

- **V1 coverage and order.** Expanding every call of section 1 in the new driver into the moved
  function's body (the lines below `// ---- End of the bindings ----`) reproduces the frozen
  function's statement lines exactly once each, in order, up to D1 to D8.
- **V2 brace balance.** Every new function, and every moved body taken alone, is
  bracket-balanced and ends at its own closing brace.
- **V3 bindings.** Every frozen-scope name a moved body uses is bound once in its new function;
  the declarator's type text, reference category and constness equal the frozen one up to D3,
  and the initializer resolves to the same root path up to D2.
- **V4 effects order.** The ordered list of logger calls (`log->`), event writes (`emitEvent(`,
  `traces.emitEvent(`), trace stream writes (`<<` on `traces.`) and `return` statements per moved
  body equals the frozen range's (D1 applied).
- **V5 gates.** Each section 2 row: the driver line has no gate, the seam's first statement
  after its bindings is the declared gate.
- **Negative controls** (each must fail before the real run is trusted): a swapped pair of lines
  in P1, a dropped line in G1, a const added to a written binding, a reordered log call in T2, a
  gate left out of one seam.

`python scripts/nom_epoch_lint.py` passes; `tests/test_network_epoch_contract.cpp` gets a row per
new or changed epoch writer (P1, F1, F2) and per retired adapter.

## 5. The slip funnel

`NetworkSlipFunnel` moves from `app/NetworkEpoch.hpp` to `filter/NetworkSlipFunnel.hpp`, with
its producers (F1, `networkPassASlipScan_` into the raw subset, F2, `networkSignalIntegrity`,
`networkMeasurementAdmission`) and readers (P1, G1, `networkPassB_`, `networkBuildCarryDdPriors`,
`networkSolveGlue`'s replay, `networkEmitStore`, `networkSignalRosterAudit`) documented in its
banner. The standing invariants it turns on (CLAUDE.md; card NOM section 3.5 cites invariant 7):
1 (every stiff carried quantity has a wired slip response) and 7 (a change in which information
reaches which states is an event at both edges, with counters and identities). Enforced without
an output change: the opening consumes the deferred flags one-shot (F1 is their only reader and
clearer), and after F2 the raw subset is contained in the funnel. Named and left unenforced,
because enforcing either adds output: a per-entry check that every carried quantity keyed by a
funnel entry was dropped, refloated or re-seeded (invariant 1), and entry and re-coupling counters
per identity (invariant 7).

## 6. Adapters retired, and the fixture

`tests/support/NetworkTestRun.hpp`: owns a `GnssConfig`, its `NetworkRunContext`, the traces
(no stream opened), the setup, a `NetworkRun` from `networkEngineSpec`, one `SyncedEpoch` and a
rebuildable `NetworkEpoch`; documented for step HV1. Retired (the explicit form becomes the
epoch form's body, its parameters becoming bindings initialized by the adapter's own arguments; the
tests call the epoch form through the fixture): `networkSignalIntegrity`,
`networkMeasurementAdmission`, `networkHandoverStransformBeforeRows`, `networkCarryDecide`,
`networkSolPrep`, `networkPbHoldCloneUpdate`, `networkPbHoldCloneHarvest`,
`networkPhaseVisibleMonitor`, `networkLedgerEmitFold`. Kept (file owned by step SG):
`networkPrefitCensus`, `networkRhsAttribTrip`. An adapter whose test sets an explicit argument the
epoch form derives from a knob (for example `carry_invariant`) gets that knob through the
fixture's configuration; where no configuration reaches the tested value, the adapter is kept and
named.

## 7. Commits

(1) this file; (2) the cut, by `scripts/nom_ep2_cut.py` (text moves from the frozen file and the
templates of section 1) with `scripts/nom_ep2_verify.py`; (3) the fixture and the retired
adapters; (4) CLAUDE.md's routing paragraph (driver line count, seam list, the slip funnel line).
