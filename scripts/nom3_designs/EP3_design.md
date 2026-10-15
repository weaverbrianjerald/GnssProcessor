# NOM3 step EP3: pre-registration (the epoch observers as a declared table)

Card: `docs/refound/NOM3_network_giants_2026-10-09.md` section 3, EP3. Base: branch `nom/wave1` `8f4f1db`
(main `7141240` plus EP2, SG1-SG3, PB1a). Frozen text: `git show 8f4f1db:src/core/app/ApplicationNetworkEpoch.cpp`
(728 lines; `Application::networkProcessEpoch_` lines 155 to 726). Branch `nom/ep3`. Pure refactor: output
byte-identical (gate G-EP, run by the orchestrator). No printed text moves, so no `reverse.sed`.

## 1. The table

`filter/NetworkEpochObservers.{hpp,cpp}` (new), modelled on `filter/NetworkRunEnd.hpp`:
`kNetworkEpochObservers`, a `std::array<NetworkEpochObserverRow, 21>` of rows
`{name, what, home, slot, observe}` (`observe` is `NetworkEpochObserver`, the alias EP1 declared in
`app/NetworkEpoch.hpp`). `NetworkObserverSlot` names the spine point; `networkObserveEpoch(run, ep, slot)`
walks the rows of one slot in table order. The table is in spine order (slots non-decreasing).

| # | row (seam) | frozen line | slot | driver gate today -> after |
| --- | --- | --- | --- | --- |
| 1 | `networkParamInventoryNoteSatellites` | 242 | `Admitted` | none |
| 2 | `networkLevelAuthorityCensus` | 355 | `BeforeColumns` | none |
| 3 | `networkPhaseVisibleMonitor` | 568 | `AfterClockQc` | none |
| 4 | `networkLedgerEmitFold` | 588 | `RosterTail` | none |
| 5 | `networkSecondPhaseEpochTail` | 596 | `RosterTail` | none |
| 6 | `networkChannelAllEpoch` | 601 | `RosterTail` | none |
| 7 | `networkMultiGnssEpoch` | 606 | `RosterTail` | none |
| 8 | `networkParamInventoryEpochTail` | 610 | `RosterTail` | none |
| 9 | `networkContributionsEpochTail` | 614 | `RosterTail` | none |
| 10 | `networkPostfitSigma0` | 633 | `FrameDiag` | none (in the seam since EP2) |
| 11 | `networkShSpectrum` | 634 | `FrameDiag` | none |
| 12 | `networkStationBlock` | 640 | `FrameDiag` | none |
| 13 | `networkPrefitCensus` | 647-648 | `FrameDiag` | `ctx.knobs.pc_walk_on`: already the explicit form's first line; driver test dropped |
| 14 | `networkRedundancyCensus` | 655 | `FrameDiag` | none |
| 15 | `networkBlockBaardaEmit` | 662 | `FrameDiag` | none |
| 16 | `networkCongruenceEmit` | 670 | `FrameDiag` | none (PB1b's file: referenced only) |
| 17 | `networkMdbFlush` | 679 | `FrameDiag` | none |
| 18 | `networkDatumGraphCensus` | 688-689 | `FrameDiag` | `ctx.knobs.dg_on`: already the seam's first line after its bindings; driver test dropped |
| 19 | `networkRhsAttribution` | 690-691 | `FrameDiag` | `est.attribArmed()`: moved to the seam's first line after its bindings |
| 20 | `networkRhsAttribTrip` | 697 | `FrameDiag` | none (SG kept its observer form) |
| 21 | `networkAmbStateTrace` | 714 | `StashTail` | none |

Not rows: `networkGapResumeCensus` and `networkBufferedTraceDrains` (EP2) have the observer form but are not among
the card's 21 (the card's list predates EP2); they stay explicit spine calls. Adding them is one row each.

## 2. The slots (spine points) and the order constraints

| slot | spine point | why there |
| --- | --- | --- |
| `Admitted` | after `networkAdmitEpoch`, before the section-timer baseline | counts every processed epoch before any model reads it |
| `BeforeColumns` | after `networkReceiverPhaseBiasAllocation`, before `networkAssemblyRegisterColumns_` | after PASS A's CS-GF advance; an empty-system epoch (SkipEpoch) still gets its census row; upstream of every withhold |
| `AfterClockQc` | after `SB_CLOCKUPD` closes, before `SB_ROSTER` opens | after the epoch's last re-solve, while the fold capture still holds the prior (the stash overwrites it) |
| `RosterTail` | in `SB_ROSTER`, after `networkSignalRosterAudit` | the audit advances the ledger's admitted machine the fold reads; `free_cause` (incl. SB_RAIM) and the anchors are final; `present` is final |
| `FrameDiag` | in `SB_FRAMEDIAG`, after the two predict halves | predict-then-update (sigma0 through the previous epoch frozen first); before the stash, whose epoch-status row reads the post-fit sigma0 and the prefit scalars |
| `StashTail` | in `SB_STASH_STATUS`, after `networkClockLevelMonitor`, before `networkBufferedTraceDrains` | first point after every carry-state row source (leveling harvest, gap prune, T4/T3 refloats, anchor re-reference) |

Within `RosterTail`: the fold after the audit (spine); the parameters-as-armed table before the
contributions-as-armed table (log and manifest order); the others kept (their counters are disjoint, their
lines and events interleave in this order).
Within `FrameDiag`: post-fit sigma0 before the MDB drain (scaled variant); the prefit walk before the
redundancy census (the free tier it escalates from); the redundancy census before the block-Baarda, congruence
and MDB drains (it feeds them) and before the attribution (it reads the unconsumed trip window); the
block-Baarda and congruence drains free relative to each other (kept); the datum-graph census free (kept last
among the censuses); the attribution, then the flight-recorder trip last (it consumes every trip raised earlier
and must see the kernel state the others read).

## 3. The cut (one commit)

- Spine: each maximal run of consecutive observer calls becomes one `networkObserveEpoch(run, ep,
  NetworkObserverSlot::X);`; the three driver gates go (13 and 18 redundant, 19 moved); the binding
  `NetworkEstimator& est = run.engines.est;` goes (its only reader was gate 19). The position comments become the
  header's order constraints; each slot call keeps a one-line pointer.
- `NetworkRhsAttrib.cpp`: one statement added after the bindings of `networkRhsAttribution`:
  `if (!est.attribArmed()) return;` (`est` is already bound there).
- `tests/test_network_epoch_observers.cpp` (new suite `NetworkEpochObservers`): the 21 rows pinned by name,
  function, slot and order; slots non-decreasing; every field filled; homes are `.cpp`.
- `tests/test_network_epoch_contract.cpp`: one line, the new spine seam `networkObserveEpoch`'s signature.
- `CMakeLists.txt`: two lines (the table's `.cpp`, the test). `CLAUDE.md`: the routing rule and line count.

## 4. The verifier (`scripts/nom_ep3_verify.py`, reduced G-R, a declared deviation)

- V1 spine: the frozen spine's statements, with each slot's observer run replaced by its slot call and the
  declared drops (three gates, the `est` binding), equal the new spine's statements in order, each exactly once.
- V2 table: the rows in `NetworkEpochObservers.cpp` equal the frozen spine's observer calls in order, with the
  slot of the run each sits in; each row's `name` is its function.
- V3 gates: the prefit explicit form's and the datum-graph seam's first statements after their bindings are the
  dropped driver gates negated; the attribution's new first statement is gate 19 negated; every binding of the
  three is a reference, pointer or copy of a run or epoch member (no side effect before the gate).
- V4 bodies: every other function of the touched seam files is unchanged statement for statement; the
  attribution changes by exactly the one statement; brace balance per function.
- Negative controls (must fail first): two rows swapped; a row in the wrong slot; a slot call moved one
  statement; the attribution gate removed; a spine statement duplicated.
- `python scripts/nom_epoch_lint.py` passes.
