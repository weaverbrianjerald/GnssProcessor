# RS-6a: station-layer memories across the seam (rx-CB carry, T5 deviation carry)

**STATUS**

- **id:** RS-6a
- **topic:** resume-seam
- **status:** closed
- **summary:** Both knobs RECOMMEND_SCOPED: the T5 deviation half removes the SH a₀₀ seam transient (+4.62 to +0.43 TECU), the rx-CB half helps modestly; most P8a/P8b bars FAIL.
- **dates in the text:** 2026-09-12 → 2026-09-13
- **knobs / posture:** `network.state_snapshot_station_capture` (write, pinned on the goldens of the time, retired 2026-09-15), `network.resume_station_restore` (read, harness-armed; sub-switches `resume_station_restore_rxcb` / `_shdev`), `network.resume_border_restore` (pre-registered here as RS-7); probe `zwd_seam_reseed` off (mixed, not the residual's cause)
- **hashes named in the text** (commit / exe / byte-cert): `ba6be47`, `a726206`, `cb4fca4`, `87a33bf0`, `545b8bdd2b638231`, `c45e52a0`
- **run dirs named in the text:** `runs/evseed_field6h_v5`, `runs/evseed_field6h_v5raw`, `runs/evleg_close3_null_main`
- **source:** `docs/archive/SPEC_event_harness.md` §21.13, §21.14, §21.15

**Outcome.** The question: do the two station-layer memories a warm resume restarts cold hold the seam residual left after RS-5? The halves partition cleanly: T5 owns the a₀₀ transient (cold seeds 400 to 50), rx-CB the receiver-clock move at +1 (1.21 to 0.91 m). a₀₀ and the PB wire PASS; raw PB, receiver clocks, code bias, clock contrast, ZWD and consumer code re-references (22, bar 5) FAIL, with the rx-CB gate still at 26 %. RS-7 FALSIFIED the dense-border attribution and RS-9 closed the gate's frame ([RS-7 … RS-9](RS-7-to-RS-9-border-and-rxcb-frame.md)); seams remain an owed add-back under the [re-founding](../../refound/NETWORK_REFOUNDING_2026-09-14.md).

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_event_harness.md -->
### 21.13 RS-6a pre-registered — the station layer's own memory across the seam: the rx-CB carry and the T5 iono deviation carry (2026-09-12 ~23:00 local, manager; before any leg is read)

**What is left at the seam after RS-5, and where it lives.**  With the emit layer restored the phase-bias WIRE
is continuous, but the estimator's own level at the first post-resume epoch still moves: raw phase bias 1.70 m
p50 (§21.10; per-station medians MAJU00MHL 2.3 / MBAR00UGA 0.96 m — a per-station component), W6 code bias
0.47 m p50 (the 21 code-bias re-references a stateful consumer takes at the seam, §21.11), receiver clocks
1.21 m p50 / 5.11 p95, clock contrast 0.54 m at +2 growing to 1.7 m pooled, ZWD 0.64 m, SH a₀₀ +4.6 TECU
decaying over five epochs.  Two station-layer memories restart cold at the seam and neither is in the
snapshot: the **rx-CB carry** (`ctx.carry.rx_cb_carry`, `std::map<std::pair<int,std::string>, LocalCarry>` —
the per-(station, code key) receiver code-bias memory with RW q 1e-9 m²/s; on a leg its 10σ inject gate
withholds 26 % of priors vs 13 % on the seed) and the **T5 per-LOS iono deviation carry**
(`ctx.carry.sh_dev_carry`, `std::map<std::string, ShDevCarry>` keyed "STATION|SAT"; 400 cold seeds at +1).
The rx-CB restart re-splits code bias / iono / clock per station and signal, which is exactly the shape of the
raw residual (per-station, not iono-ratio'd); the deviation restart is the a₀₀ transient.

**Build (default-OFF, byte-identical when off, forward-only, not fingerprinted — the Z-row precedent).**
Write knob `network.state_snapshot_station_capture`: rows `RC,<station index>,<cb key>,<value_abs>,<var>,
<last_seen_s>` (the same `LocalCarry` fields the Z rows serialize, one per rx-CB carry entry) and
`SD,<STATION|SAT>,<every ShDevCarry field>` under the existing header.  Read knob
`network.resume_station_restore` (requires `resume_frame_restore`): restore the rx-CB entries through
`makeRestoredLocalCarry` (the restored-arc sentinel, §15.4.1) and the deviation entries as they were, before
the first epoch; either half absent ⇒ inert and logged; counters + run-end line.  Two sub-knobs are NOT wanted
— the halves are gated by which rows the snapshot carries and by two leg-side switches
`resume_station_restore_rxcb` / `_shdev` (default true; diagnostic only) so one leg can restore one half.

**Instruments.**  The raw estimator level is visible only with the two outputs-only emit transforms off
(`carry_invariant_emit_rebase: false`, `emit_pb_filter: false` — not fingerprinted, §21.10), so the legs come
in two seeds written by the same exe from the current golden: `runs/evseed_field6h_v5` (the golden + the write
knob; carries every row kind of the day — the harness seed from now on) and `runs/evseed_field6h_v5raw` (v5
+ the two transforms off; the estimator-level instrument).  Legs, 60 epochs from ep0604, one exe:
from v5raw — `rs6_raw_ctl`, `rs6_raw_both`, `rs6_raw_rxcb` (rx-CB half only), `rs6_raw_shdev`;
from v5 — `rs6_null_ctl`, `rs6_null_both` (+ the stateful-consumer splice on both).

* **P8a the raw level (v5raw legs, products = raw state):** raw PB leg − seed at +1 p50 ≤ 0.5 m (from 1.70;
  excluding the six L2X keys whose datum receiver drops); receiver clocks leg − seed at +1 p50 ≤ 0.3 m
  (from 1.21); SH a₀₀ at +1 within 2 TECU (from +4.6); the rx-CB gate's withheld share on the leg within 3
  points of the seed's 13 %.
* **P8b the wire (v5 legs):** W6 code bias (B records) leg − seed pooled p50 ≤ 0.10 m (from 0.47); clock
  contrast pooled p50 ≤ 0.5 m (from 1.74) and at +2 ≤ 0.2 (from 0.54); ZWD p50 ≤ 0.32 m (from 0.64); PB wire
  unchanged (0.000 at +1, RS-5); the stateful consumer's code-bias re-references at the seam ≤ 5 (from 21)
  and its phase-bias resets 0.
* **P8c attribution:** the rxcb-only and shdev-only legs partition the P8a moves; the half that moves a
  channel owns it.  Both halves passing their share ⇒ RS-6 closes; a residual left with both restored names
  the next memory (the receiver phase-bias states, the six L2X families' datum receiver).
* Byte-identity: write knob on/off products identical; a cold run with the read knob inert; suite green.

### 21.14 RESULTS — RS-6a: the two station-layer memories partition cleanly, the T5 half removes the a₀₀ seam transient; the persistent residual is not `zwd_seam_reseed`; RS-7 pre-registered (2026-09-13 ~03:00 local; worker on worktree-agent-adf16ebd10c1b98dc `ba6be47`, merged `a726206`, registered `cb4fca4`; manager grade)

**Build.**  `RC` rows (the Z rows' `LocalCarry` field set — complete for this carry: its harvest stashes exactly
`value_abs / var / pl_now_s` and its inject reads only those) and `SD` rows (the four `ShDevCarry` scalars);
restore after the RS-5 emit restore; the rx-CB entries through the restored-arc sentinel (inert for this carry
today — no reader tests an age — locked by a test; the load-bearing field is `last_seen_s`, which the gap
inflation `var + q·Δt` and the 900 s prune key on) and the deviation entries as written (their harvest stamp
`t_s > 0` is what makes them read WARM; the aging pre-pass ages them over the true gap and drops them at 5τ);
two diagnostic sub-switches partition the halves.  Cold smoke `products.txt` **`87a33bf0…` both arms**,
snapshot +61 RC / +376 SD rows; Debug suite 2013 / 275 suites / 0 failed; `ResumeStationRestore` (12).
Seeds `runs/evseed_field6h_v5` (the harness seed from now on: every row kind of the day; its products are v4's
with the serialized counters, 0 differing values) and `v5raw` (emit transforms off — the estimator instrument);
ep0604 at the same tow, fingerprint `545b8bdd2b638231` unchanged.

| leg − seed, p50 (p95) | raw ctl | raw both | raw rx-CB only | raw T5 only |
|---|---|---|---|---|
| raw PB at +1, ex L2X | 1.68 (4.06) | **1.50 (2.83)** | 1.46 (3.82) | 1.72 (3.03) |
| raw PB at +2, ex L2X | 1.75 (4.66) | 1.51 (3.34) | 1.56 (4.55) | 1.63 (3.66) |
| receiver clocks at +1 (61 sta) | 1.21 (5.11) | 0.93 (5.70) | **0.91 (5.10)** | 1.12 (5.31) |
| SH a₀₀ at +1 / +6 (TECU) | **+4.62 / +0.46** | **+0.43 / +0.11** | +4.71 / +0.44 | **+0.32 / +0.11** |
| T5 cold seeds over the leg (seed: ambient) | 400 | **50** | 400 | 50 |
| rx-CB gate withheld share (seed, same window: **11.0 %**) | 26.2 % | 26.3 % | 26.3 % | 26.2 % |

| wire (v5 legs), leg − seed | null ctl | null both |
|---|---|---|
| PB wire at +1 / +2 / +30 p50 | 0.0004 / 0.0008 / 0.038 | 0.0004 / 0.0007 / 0.048 |
| W6 code bias at +2 / pooled p50 | 0.128 / 0.472 | 0.152 / 0.463 |
| clock contrast at +2 / pooled p50 | 0.441 / 1.593 | **0.291** / 1.503 |
| ZWD pooled p50 / p95 | 0.625 / 4.97 | 0.642 / 5.00 |
| stateful consumer at the seam: PB / code / clock re-refs | 0 / 21 / 0 | 0 / 22 / 0 |

**Verdicts.**  P8c the partition is clean: the T5 deviation half OWNS the SH a₀₀ seam transient (+4.62 → +0.43
with both, +0.32 alone; rx-CB alone +4.71; cold seeds 400 → 50 = the seed's own ambient rate) and halves the +2
clock contrast; the rx-CB half owns the receiver-clock move at +1 (1.21 → 0.91 m) and the raw PB tail (p95
4.06 → 2.83 m).  P8a: a₀₀ PASS (≤ 2 TECU); raw PB p50 1.50 (bar 0.5) and receiver clocks 0.93 (bar 0.3) FAIL;
the rx-CB gate share does not move.  P8b: PB wire unchanged at 0.000 (PASS); code bias 0.46 (bar 0.10), clock
contrast pooled 1.50 (bar 0.5), ZWD 0.64 (bar 0.32), consumer code re-references 22 (bar 5) FAIL.  Both knobs
RECOMMEND_SCOPED, the write knob pinned on the goldens, the harness arms both (`cb4fca4`) — the T5 half is a
clean win and the rx-CB half a modest one; neither is the residual.

**The residual is structural, and `zwd_seam_reseed` is not it (probe `rs6_{raw,null}_both_nozwd`).**  The
rx-CB gate withholds 26 % of its priors on EVERY leg — the whole 60 epochs, not a transient — against 11.0 %
on the continuous seed over the identical window; the code-bias level (0.46 m), ZWD (0.6 m) and the pooled
clock contrast (1.5 m) sit with it.  The one thing every leg had that the seed never has, the harness's ZWD
seam re-seed (61 Z rows as B9 priors at q 1e-9 for 480 epochs — validated on the sim, §17, never on the
field, where the ZWD channel is the datum sink), was switched off on both postures: ZWD 0.64 → 0.41 m (p95
5.0 → 2.6), receiver clocks 0.93 → 0.75, the raw PB tail at +30 15.3 → 9.1 m and the pooled clock-contrast
p95 9.0 → 5.1 improve; a₀₀ at +1 +0.43 → +0.73 TECU and no longer decays (+0.36 at +6), the +2 clock contrast
returns to the control's (0.29 → 0.44), the code bias worsens (0.46 → 0.50) and D5 trips 53 at +1 instead of
38; the rx-CB gate reads 26.2 % either way and the consumer 22 code re-references either way.  Mixed on the
field, and NOT the residual's cause: the harness default stays armed and the `zwd_seam_reseed` ledger row gets
this field measurement as a scope note, with the decision deferred to RS-7 (the ZWD / DCB / clock split is
what the shared border carries, and that border is the one memory every leg still rebuilds from one epoch).

**RS-7 pre-registered — the dense shared border across the seam.**  Every leg reads `Network (C1): warm
resume restored 256 diagonal carries — the full-covariance carry re-seeds at the first emitted epoch`: the
D2 shared carry (clocks, W6 code biases, SH coefficients) comes back DIAGONAL although the seed's snapshot
carries its dense covariance (the 150 QK/QP rows of `state_snapshot_coupling_capture`), because the dense-border
restore (§18.12 (2a), `NetworkRunContext.cpp` ~:1572–1614) lives behind `resume_coupling_restore`, which the
loader refuses on this lineage for an unrelated reason (it hard-requires Card 1f, out of regime under
`carry_invariant`).  A shared border that restarts with one epoch's information behaves exactly like the
residual: a noisier per-epoch DCB / clock / SH split for as long as the border's cross-covariance is thinner
than the seed's, more 10σ withholds at the rx-CB inject, a metre-class ZWD sink and a code-bias level that a
stateful consumer re-references.  Build: split the dense-border restore out of `resume_coupling_restore` into
its own read knob `network.resume_border_restore` (default false; forward-only, fingerprint-exempt; requires
`resume_frame_restore`; restores the QK/QP dense D2 covariance for the restored D rows — the coupling ROWS
(QL/QZ regressors) stay behind the 1f interlock), counters + run-end line; write side unchanged (the rows
already exist).  Legs from `v5raw` / `v5` ep0604, one exe: `rs7_raw_ctl` / `rs7_raw_border`, `rs7_null_ctl`
/ `rs7_null_border` (+ the stateful splice).
* **P9a the border is dense from +1:** the C1 line reports the dense restore (no "diagonal" clause); first-fold
  clock trips (the §18.11 counter) at the seed's level.
* **P9b the residual closes:** rx-CB gate withheld share within 3 points of the seed's 11.0 %; W6 code bias
  pooled p50 ≤ 0.10 m (from 0.46); clock contrast pooled p50 ≤ 0.5 m (from 1.50); ZWD pooled p50 ≤ 0.32 m
  (from 0.64); raw PB at +1 p50 ≤ 0.5 m ex L2X (from 1.50); receiver clocks at +1 ≤ 0.3 m (from 0.93); the
  stateful consumer's code re-references at the seam ≤ 5 (from 22) with phase resets 0.
* **P9c** PB wire unchanged at 0.000 (RS-5); a₀₀ within 2 TECU; the `zwd_seam_reseed` pair re-run on the
  border-restored posture decides that knob's field scope.

### 21.15 FINAL CERTIFICATION of the day's tree (2026-09-13 ~04:30 local; worker on main at `cb4fca4`; docs commits after it only)

Debug and Release rebuilt with the wrapper, first attempt.  **Full Debug suite on main: 2013 cases / 275 suites,
2012 passed, 1 skipped (the documented fixture gate), 0 failed.**  Smokes on `gnss_processor_close3.exe`
(3E83D30A…): the golden as it ships and the same with all four write knobs (`state_snapshot_carry_lin` /
`_datum_capture` / `_emit_capture` / `_station_capture`) false both hash `products.txt` **`87a33bf0…`** — the
write side of every card is byte-identical; the snapshots separate exactly by knob (`base` writes RC 61 / SD
376 / ER 135 / PF 135 / ZL 4 / EQD 6 / CBP 129; `nowrite` none of them); posture rows correct, no unknown-knob
warnings.  Closing leg `runs/evleg_close3_null_main` (v5 ep0604, `event_leg.py` defaults, which now arm every
resume knob of the day): 60/60, every seam and run-end line as §21.14's `rs6_null_both`, and its
`products.txt` **byte-identical to `rs6_null_both`'s** (`c45e52a0…`) across two independently linked binaries.

The seam card's state at close: the field golden warm-resumes with the DD branch, both datums, the emit layer
and the station layer continuous; the phase-bias wire is continuous to 0.000 m at the first epoch and the
a₀₀ seam transient is gone; a stateful consumer sees 0 phase-bias resets at the seam.  What remains is the dense
shared border, restored DIAGONAL on this lineage (RS-7, §21.14) — the code-bias level, the ZWD sink offset, the
pooled clock contrast and the rx-CB gate's 26 % share all sit with it.

