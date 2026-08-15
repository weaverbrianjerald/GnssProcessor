# TurboEdit gap bridge across a short receiver LLI

**Status:** implemented behind `estimation.cycle_slip.turbo_edit_bridge_lli` (default **off**; baseline
byte-for-byte unchanged when off). **Date:** 2026-06-11.

## Motivation — the G03 109768–773 event (SBF replay, 1 Hz)

A short data gap on G03 (L2W dropped, receiver set **LLI** at 109768–770) ended with the MW detector
spanning the gap and confirming a 25.75-cycle WL step at 109772 (`gf_m` −6.10 → +0.74 m confirms a clean
integer pair: dN1=−9, dN2=−35, ΔN_WL=26). No repair fired. Root cause, traced through the logs:

| # | step |
|---|---|
| 1 | The receiver LLI at 768 drove `CycleSlipManager`'s LLI-authority full reset, which calls `turbo_->resetSatellite("G03")` — **erasing the GF arc and its pre-gap level**, the very reference the gap bridge needs (`CycleSlipManager.cpp`, the `lli_sat` branch). |
| 2 | The state side reset/pruned G03's ambiguities at 768 → converged precision already gone. |
| 3 | When L2W returned at 771 the GF arc warmed up *fresh* (no bridge), reaching its first TD2 only at 773 — by which point the MW arc had reset and emitted nothing. The MW flag (771/772) and GF readiness (773) never coincided. |

The gap bridge exists to span exactly this, but its design **deliberately dies on LLI** ("a
receiver-declared loss of lock voids the level reference"). This feature makes that optional for AR-able
dual-frequency signals at high rate.

## Key caveat

A lossless integer repair beats a reset **only if the carrier tracking loop held phase coherence across
the LLI** (data gapped / LLI flagged conservatively, but cycle count kept). If the receiver truly
re-acquired, the new integer is arbitrary, the fresh post-gap seed is already correct, and the reset was
right. The clean integer fit does **not** distinguish the two (any re-acquired phase fits *some* integer
pair). The bet is backstopped by the KF phase innovation gate: a wrong held-repair lands ≥|dN·λ| in that
signal's innovation when it rejoins the update and is reset one epoch late — the same cost as the
re-acquisition path.

## Mechanism (soft-LLI hold → bridge repair)

At a qualifying LLI epoch (toggle on; `turbo_edit` + bridge active; high-rate ≤ 1 s; AR-capable
constellation), instead of the immediate full reset:

| # | step |
|---|---|
| 1 | **Snapshot** the satellite's pre-gap ambiguity values/variances from a rolling "last good" map (captured end-of-epoch, so it holds the last converged epoch *before* the signal dropped). |
| 2 | **Preserve the GF arc** — skip `turbo_->resetSatellite` for the sat; call `turbo_->markHeldAcrossLli(sat, sig)` so the bridge treats the signal as continuous (repair, not moot) at gap return. |
| 3 | **Hold the measurement** — register the sat's phase signals in `held_pending_bridge_`; `Application` seeds the KF update's `excluded_measurements` from `getHeldPendingBridgeKeys()`, so the held (still-slipped) ambiguities never contaminate shared states across the MW one-epoch confirmation delay. |
| 4 | **At bridge confirm** (the resolver emits a repair for the held pair): overwrite the ambiguity with `snapshot_value + dN·λ` at the snapshot variance (cross-covs zeroed), release the hold → the signal rejoins the update with the correct integer-shifted value. |
| 5 | **Backstops:** if the gap exceeds `turbo_edit_max_gap_s`, or the bridge resolves to a reset/moot, or no confirm arrives within the cap, the hold is released and the normal reset stands. The state reset at the LLI epoch proceeds normally — the snapshot + measurement-hold + restore make it harmless. |
