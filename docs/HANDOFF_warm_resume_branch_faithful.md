# Handoff: branch-faithful network warm-resume — methods for review

> **SUPERSEDED (2026-07-23).** This document's §"Our interpretation" — that partial
> restoration is *counterproductive* — is **RETIRED**. It was concluded inside a
> 15-epoch window whose decay constants are themselves 15 epochs. Over 60 epochs the
> branch-faithful arm crosses below the cold arm at ep 18 and diverges from inline at
> ~half the rate. See `docs/SPEC_warm_resume_event_card.md` (review + card) and
> `docs/REPORT_warm_resume_event.md` (execution + verdict). The measurements and method
> description below are left unedited as the record of what was done.

**Purpose:** Fable to review our *methods and conclusion*, not to re-run. We investigated
whether serializing the Card-4 invariant DD carry into the RTB-7 snapshot makes a warm
resume reproduce a from-scratch run. It does not, alone — and appears counterproductive.
We want the metric and the conclusion stress-tested before deciding next steps.

## Motivation
The network producer needs hours (epoch *and* wall time) to converge, killing dev iteration.
Plan: converge once → snapshot → warm-resume for each dev run. The existing RTB-7 snapshot
restores the shared-border products (sat clock / code-bias / phase-bias / VTEC — as **per-key
diagonal** carries) + the emitter blob, but **not** the Card-4 invariant DD z-store (the
phase-ambiguity carry). Prior measurement: a warm resume lands on a **different integer/
ambiguity branch** than inline — emitted products diverge ~1 m between-sat and *grow* (not a
decaying transient). "Branch-faithful" = also restore the DD carry so the phase branch resumes
identically.

## What we implemented (default-OFF `network.state_snapshot_carry`)
- `NetworkInvariantCarryDriver::serializeCarry` / `restoreCarry`: round-trips the `carried_`
  blocks (per-component DD means `z`, **dense** `Pz`, tree/chord/vertex structure) as a
  `CARRY..CEND` blob, mirroring the emitter blob the snapshot already carries verbatim.
- Also serializes `carry_sat_vertex` (sat→vertex) and `carry_group_id` ((const,signal)→group) —
  the *stateful first-seen-order* id maps the z-store's ArcKeys are numbered by; station
  vertices are the roster index (stable under the fingerprint, no map needed).
- Requires `carry_invariant` + a snapshot path. Unit test: exact round-trip (mean/var/cross/
  structure) across multiple groups — passes. Default-off byte-identity confirmed (below).
- Files: `NetworkInvariantCarryDriver.{hpp,cpp}`, `NetworkStateSnapshot.{hpp,cpp}`,
  `NetworkRunContext.cpp` (capture in `networkWriteStateSnapshot`, restore in
  `networkTryWarmResume`), `GnssConfigTypes.hpp` + `GnssConfigLoadNetwork.cpp`,
  `ApplicationNetwork.cpp` (thread `carry_driver` into the two calls),
  `tests/test_network_invariant_carry_driver.cpp` (`SerializeRestoreRoundTripIsExact`).

## Validation method
Three runs, 50-station GPS-only, all-bias + SH15, 30 s:
- **A (inline):** cold, epochs 0–30.
- **B1 (converge):** cold 0–14, writes the snapshot (with the carry blob).
- **B2 (resume):** warm-resume from B1's snapshot, epochs 15–30.

**Metric:** for each overlapping epoch and product kind (CLK, CB), form each run's
**between-satellite single differences** `d_i = p_i − p_ref`, then the RMS over satellites of
`(d_i^A − d_i^B)`. Between-sat SD is datum-invariant (cancels the receiver-clock / per-const
sum-to-zero common mode), so A's and B2's different datum realizations cancel. **PB is
excluded** — it is integer-ambiguous, so its between-sat SD is dominated by branch integer
offsets (~16 m) and is uninformative.

**Isolation:** ran B2 with the carry toggle ON and OFF on the *same* binary vs the *same* A.

## Result (same binary, vs same inline A)
| B2 variant | chord births | slip refloats | CLK@ep15 | CLK@ep30 |
|---|---:|---:|---:|---:|
| cold (carry OFF) | 2403 | 972 | **834 mm** | 1708 mm |
| warm (carry ON)  | 1439 | 1173 | **1480 mm** | 1793 mm |

- **Default-off byte-identity holds:** new-binary carry-OFF (833.9 mm) == old-binary (834 mm).
- **The carry IS restored and used** (births 2403→1439; restore log: "restored invariant DD
  carry (1214 carried DD)").
- **But warm ≈ doubles the ep-15 divergence** (834→1480 mm) and stays worse, with **+201 slip
  refloats** (1173 vs 972).

## Our interpretation (the claim to review)
Partial state restoration is **counterproductive**: the warm DD carry is inconsistent with the
cross-epoch state that is *not* restored —
1. the **slip-detector pools** (IF-phase TD, MW) cold-start and read the warm carry's
   innovations as slips → spurious refloats that discard the restored DDs (the +201 refloats);
2. the **C1 dense shared-border covariance** is restored diagonally, not densely.

So branch-faithful resume looks **all-or-nothing**: DD carry **+** C1 dense cov **+** slip pools,
mutually consistent. Restoring one piece yields a state the machinery mishandles.

A pragmatic alternative needs **none** of it: **same-snapshot A/B** — run baseline and variant
*both* resumed from the identical snapshot; every cold-start artifact (branch, cov, slip pools)
is shared and cancels in the change-vs-baseline delta.

## Questions for Fable (methods)
1. **Is the metric fair?** Between-sat SD-of-SD of CLK/CB is confounded by the C1 shared-cov gap
   (which the DD-carry change does not address) and cannot isolate the *phase* branch. Better
   candidates: diff the carried DD values directly (datum+integer-invariant), a state-level diff
   of A@ep15 vs B2@ep15, or the closed-loop consumer position. Does the CLK increase reflect a
   real degradation or a confound?
2. **Is "partial restoration is counterproductive" causally sound?** +201 refloats is suggestive,
   not proof. Is the cold-slip-detector → spurious-refloat chain the actual mechanism?
3. **Is all-or-nothing the right framing?** Would restoring C1 cov + slip pools close it, or are
   there further un-serialized cross-epoch states (per-station iono, PB emission filter, pivot
   handover, D5 σ̂₀)? Which are re-estimated per-epoch (safe) vs carried (must restore)?
4. **Is same-snapshot A/B a sound dev substitute**, or does it hide transients that would bite
   real numerical grading?

## Recommendation (pending review)
Do not ship DD-carry-only (counterproductive alone). Either revert, or keep default-off strictly
as groundwork *if* we commit to the full all-or-nothing serialization. Use same-snapshot A/B for
dev iteration now. All changes are default-off; the normal producer path is byte-identical.
