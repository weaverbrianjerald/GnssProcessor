# RS-2 — the restored invariant DD branch is folded in the WRITER's linearization frame — Phase 1/2 and RS-2b

**STATUS**

- **id:** RS-2
- **topic:** resume-seam
- **status:** closed
- **summary:** the restored invariant DD branch is folded in the WRITER's linearization frame — Phase 1/2 and RS-2b
- **dates in the text:** 2026-08-02 → 2026-09-12
- **knobs / posture:** `network.carry_coupling_gauge_project`, `network.carry_conditional_coupling`, `network.carry_invariant`, `network.state_snapshot_carry_lin`, `network.resume_carry_rebase`, `network.resume_d5_warmup_ep`, `network.state_snapshot_datum_capture`, `network.resume_datum_restore`, `network.closed_loop.replay_products_path`
- **hashes named in the text** (commit / exe / byte-cert): `545b8bdd2b638231`, `eb37b24`, `cf08909`, `a35d2254`, `2c62724`, `62303d2`, `b903b66`, `b9da8cc1` … (+9 more in the text)
- **run dirs named in the text:** `runs/evseed_field6h`, `runs/evleg_fld_null_off`, `runs/evseed_field6h_v2`, `runs/evleg_rs_null_`, `runs/evleg_rs2_null_`, `runs/evleg_rs2_null_rebase_d5w`, `runs/evseed_field6h_v3`, `runs/evleg_close_null_main`
- **source:** `docs/archive/SPEC_event_harness.md` — 21, 21.1, 21.2, 21.3, 21.4, 21.5, 21.6, 21.7, 21.8

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_event_harness.md -->
## 21. THE FIELD WARM-RESUME SEAM (card RS-2) — the restored invariant DD branch is folded in the WRITER's linearization frame; §20.1's "Card 1f is the prerequisite" is WITHDRAWN (pre-registered 2026-09-12 ~07:30 local, manager; before any new leg is read)

### 21.1 What §20.1 called DEFECT 1, re-read from the instruments the legs already wrote

§20.1 attributed the field golden's inability to warm-resume (every leg from `runs/evseed_field6h`
ep0604 withheld 61/61) to the missing Card 1f `carry_coupling_gauge_project`, on the strength of the
loader's own text at `GnssConfigLoadNetwork.cpp:3602–3609`.  That attribution is withdrawn:

* **1f is OUT OF REGIME on this lineage.**  Ledger row `network.carry_coupling_gauge_project`:
  `regime: on("network.carry_conditional_coupling") and not on("network.carry_invariant")`; the
  field golden runs `carry_invariant: true`, and `ApplicationNetworkAssembly.cpp:3117` gates the
  whole leveling inject on `do_amb_carry = (…) && !carry_invariant` — the Card-1 coupling rows the
  loader text is about are never built here (the 1g ledger row says the same for every
  `carry_invariant` lineage: "structurally INERT").  Arming 1f would append a fingerprint token and
  change nothing.  The `resume_coupling_restore` interlock that names 1f is therefore not a
  diagnosis of this seam; it is an interlock for a different vehicle.

* **The first post-resume solve is torn apart by ONE prior class.**  The rhs-attrib flight
  recorder fired on the SH-gate trip at the leg's first epoch (`runs/evleg_fld_null_off/logs/
  rhs_attrib/attrib_277320.csv`, TOTAL rows, `norm_total`):

  | class | norm_total | c00_l1 / c00_l2 |
  |---|---|---|
  | **dd_fold** (the restored invariant DD carry's prior) | **7.82e7** | **−1377.05 / −3471.85** |
  | pivot_pin | 2.44e6 | −1.56 / −1.11 |
  | cpl_row (the 61 ZWD coupling rows) | 5.14e5 | 1.01 / 0.58 |
  | shared:d2_leftover | 8.51e3 | 0 / 0 |
  | obs_code — every code observation of 61 stations | 616.0 | 0.72 / 0.46 |
  | obs_other | 233.8 | |
  | shared:d2_sh / iono_sh_row | 190.3 / 121.1 | |
  | obs_phase | 0.0008 | |

  Five orders of magnitude above the data, and it drives a₀₀ directly (the c00 columns).  Epoch
  status at 277320 (leg vs the seed at the same tow): a₀₀ **−11109.1** vs **+38.33 ± 0.80** TECU,
  `clock_std_m` **2819.5** vs 9.88, `sigma0_pf_code` **17218.8** vs 4.886, `sigma0_pf` 1564.7 vs
  0.137, worst sat G20 6868.7 m; `n_d5_flags` **676 / 676 re-taps** (seed: 0), `n_chord_births` 64;
  at the second epoch **674 births / 672 slip refloats** (the raw detectors, now with two samples,
  erase the branch wholesale), `clock_std` still 2681 m.  The SH gate never releases: a₀₀ −1350 at
  +20, −1162 at +40, −1198 at +60.  The prior-residual census over the leg: `zwd` σ̂₀ **12515**
  (worst |ê| **4177 m** at SGOC00LKA against `fold_var` 7.1e-5), `cb` 1604, `rxcb` 84.6; the rx-CB
  carry withheld 3624 of 3660 priors at its 10σ gate.  The plain RTB-7 leg (`evleg_fld_null_plain`,
  no frame restore) is the same failure: a₀₀ −14113.6 at the first epoch, one-epoch releases at
  +40/+42 (145.2 / 82.6 TECU), re-engaged (352.6, −94.2), 6 epochs emitted.

* **Restore mechanics were textbook in every leg** (`stdout.txt:414–418`): 256 D2 shared-carry
  keys (restored DIAGONAL — "the full-covariance carry re-seeds at the first emitted epoch"), 0
  ambiguity carries (none exist under `carry_invariant`), 61 ZWD Z rows, **701 carried DD restored
  branch-faithfully** (RTB-7c, σ̂₀ seeded 2.8259, Pᶻ dense v2 blob, binding validated), 126
  phase-anchor incumbents + 32 CLK-R held-frame keys, snapshot age 30 s, generation id preserved.
  The blob at ep0604 is `CARRY,v2,4` (4 blocks, 701 chords).

### 21.2 The mechanism (read from the code; the pre-registered legs test it)

`NetworkInvariantCarryDriver::serializeCarry` (`NetworkInvariantCarryDriver.cpp:182`) writes per
block the `Z` row (chord means ẑ), the `P` rows (Pᶻ), and the `H/X/A` topology rows — **nothing
about the linearization**.  The chord mean is the CORRECTION-frame double difference:
`SPEC_warm_resume_event_card.md` §4 already states "the serialized `z` is the CORRECTION-frame DD
(λ·N minus Σc·x_lin), hence run-specific" — and drew from it only the conclusion that the
branch-agreement METRIC was invalid across runs.  The stronger consequence was never drawn: **a
resumed run re-seeds every station's ambiguity `x_lin` from code at its first epoch
(`cold_clock_seed` + the code-minus-phase seed; the station layer is not snapshotted), the DD prior
fold reads THAT `x_lin` per arc (`NetworkDdPriorBuild.cpp:254–266`, `lin_of`) and forms
`v = z_free − Gᵀx_s − ẑ` (driver `:1770`) — so every restored ẑ is wrong by Σc·(x_lin,writer −
x_lin,now)**: the DD of code-seeded ambiguity values taken hours apart, metres per chord, folded at
q_z = 1e-4 m²/s stiffness on 701 chords at once.  The D5 innovation gate is POST-solve, so it can
only flag the damage (676 flags), not prevent the fold; and `carry_invariant_innov_corroborate`'s
"no evidence still re-taps" keeps the levels for the next epoch anyway.  By the second epoch the
raw detectors erase the branch — but the shared memory (the SH carry, the D2 clocks/biases, the
rx-CB carry) has already been stashed from the garbage solve, which is why nothing heals: the
withhold era is self-sustaining.

**Why this was invisible on 2026-08-02 (`ft63_warm12`, "resume was textbook, the two arms are the
same run after ~3 epochs").**  `config/test_suite/_ft63_warm12.json` arms `carry_invariant` +
`state_snapshot_carry` + the D5 gate but **not `carry_invariant_pd_safe`** — it ran the OLD fold,
behind which "78 % of the carried state was measured diffusing" under a silent M:=0 freeze
(`SPEC_pd_safe_dd_fold.md`).  A frame-wrong branch that mostly cannot bite produces exactly what was
recorded: a 91 m re-entry spike that dies in 2–3 epochs, 587 of 769 restored chords reborn in the
first two epochs, and a "benefit window" of ~3 epochs.  The golden pins the PD-safe fold, so the
restored branch now bites at full stiffness.  **Interpretation, not yet a measurement** — it is
consistent with every number above and is the reason the RS card's "branch-faithful" PASS cannot be
carried over to the golden posture; it is NOT graded here (pd_safe is fingerprinted and cannot be
A/B'd on a leg).

### 21.3 Pre-registered legs — Phase 1, on the EXISTING seed (no new seed, no code change)

All legs: `runs/evseed_field6h` ep0604 (tow 277290; fingerprint 545b8bdd2b638231), 60 epochs
(tows 277320…279090), NULL (no injected event), one exe built from main at or after eb37b24,
`--extra` always carrying `"resume_coupling_restore": false` (the loader throws otherwise on this
lineage), run one at a time.  Every discriminator below is fingerprint-EXEMPT (checked against the
token list `NetworkRunContext.cpp:644–978`): `state_snapshot_carry` is the read-side gate of the
blob restore (`:1724`) and not a token; `sh_health_gate` and `zwd_seam_reseed` and
`resume_frame_restore` are QC / forward-only resume policy.

| leg | knobs beyond the standard leg | question |
|---|---|---|
| L0 `rs_null_ctl` | none | same-exe control: must reproduce §21.1 (61/61 withheld, a₀₀ < −5000 at +1, `dd_fold` TOTAL > 1e7) |
| **L1 `rs_null_nocarry`** | `state_snapshot_carry: false` | **the blob is the cause**: shared / Z / frame restored warm, the DD branch cold-starts |
| L2 `rs_null_plain_nocarry` | `state_snapshot_carry: false`, `resume_frame_restore: false` | is the frame restore needed for availability on the field, or only for the seam's PB continuity |
| L4 `rs_null_shgateoff` | `sh_health_gate: false` (blob restored) | the deadlock question: with the wire allowed to emit, does a₀₀ return to the seed's band by +60 (the era is the gate's) or not (the pollution is in the carried memory) |

**Gates (bars frozen now).**  The seed's own traces at the matched tows are the continuous
reference (`network_epoch_status.csv`, `network_vtec_sh.csv` layer-0 (0,0) = a₀₀, `network_zwd.csv`,
`products.txt`, `closed_loop_trace.csv` — 61 consumer rows exist in the window).

* **G1 (control, L0):** as above.  A control that does not reproduce voids the phase.
* **G2 (availability, L1):** emitted ≥ 55 of 61; any withheld epoch lies in +1…+5.
* **G3 (frame, L1), every epoch from +5 on:** |a₀₀,leg − a₀₀,seed| ≤ 5 TECU; `clock_std_m` ≤ 2×
  the seed's; `sigma0_pf_code` ≤ 2× the seed's.
* **G4 (branch cost, L1):** chord births per epoch from +5 on ≤ 5× the seed's rate at the same
  tows; the D5 flag count at +1 is recorded (structurally ~0 without a restored branch — the number
  that matters is Phase 2's).
* **G5 (wire + ZWD continuity, L1), from +5 on, leg − seed at matched tows:** satellite clock
  (per-epoch median over satellites removed, the datum-invariant contrast) p50 |Δ| ≤ 0.05 m and
  p95 ≤ 0.20 m; W6 code bias p50 |Δ| ≤ 0.02 m; ZWD p50 |Δ| ≤ 5 mm, p95 ≤ 20 mm; phase-bias
  re-realization at the seam either DECLARED (the emitter's disc counters bump) or absent — a
  silent |ΔPB| > 0.05 m on more than 10 % of keys at any epoch FAILS (invariant #7).
* **G6 (L2 vs L1):** recorded, no bar: whether the plain resume passes G2/G3 too, and the PB-side
  seam step with vs without the frame restore.
* **G7 (L4):** recorded: a₀₀ at +60 within 5 TECU of the seed ⇒ the era was the gate's; otherwise
  the carried memory's.
* **Consumer:** recorded, not gated in Phase 1 — the closed-loop consumer is not snapshotted and
  its own cold start dominates a 30-minute window (§18.14: grade seams with a cold-consumer replay).

**Decision rule.**  L1 passes G2–G5 ⇒ the blob is the whole seam defect; the INTERIM harness
posture for field legs becomes `state_snapshot_carry: false` in `--extra` (shared/Z/frame warm, DD
branch cold — "RTB-7(a)"), which unblocks the owed field price of the §20.1 wrong narrowing and the
scope+pairs pin work; Phase 2 builds the real fix.  L1 fails G2 or G3 ⇒ a second class is torn:
re-read the L1 first-epoch rhs-attrib TOTAL table (the recorder fires on the trip) and bisect the
remaining restored classes (Z rows via `zwd_seam_reseed: false`; frame rows via L2) before any build.

### 21.4 Phase 2 — the fix, pre-registered (built only after Phase 1 is graded by the manager)

**Design: re-base the restored branch, do not re-seed the stations.**  The carry must live in the
absolute value (the standing `x_lin`-shift-is-a-solve-no-op principle: the prior's RHS is formed
against the CURRENT linearization).  Two additions, both default-OFF and byte-identical when off:

1. **Write side, `network.state_snapshot_carry_lin` (default false).**  At every harvest the driver
   also stores per chord `zl = Σc·x_lin` (the same incidence that forms `z_free`, applied to the
   arcs' current `x_lin` — `NetworkDdPriorBuild` already has `lin_of` per arc), and
   `serializeCarry` writes one `ZL` row per block beside `Z` (format-forward: a reader without the
   row behaves exactly as today; snapshot bytes change only when armed).  Products and every trace
   byte-identical (write-side only, the frame-capture precedent).
2. **Read side, `network.resume_carry_rebase` (default false; forward-only resume response policy,
   fingerprint-EXEMPT like `resume_frame_restore`).**  A restored block whose `ZL` row is present is
   marked UNBASED; at the FIRST fold after the resume the prior build computes `zl_now` for every
   restored chord from the epoch's `lin_of` and the driver re-bases `ẑ ← ẑ + (zl_writer − zl_now)`
   (Pᶻ unchanged — an exact frame translation).  A chord with ANY arc absent at that epoch cannot be
   re-based and is DROPPED (marginalized) rather than folded frame-wrong — counted.  Armed without
   `ZL` rows in the blob ⇒ loud "branch NOT re-based" warning and today's behaviour; run-end line
   `n_resume_carry_rebased / n_resume_carry_dropped_unbased / max |Δẑ|`; the D5 flag count at the
   first post-resume epoch is the invariant-#4 readout.

`scripts/event_leg.py`: write-seed arms `state_snapshot_carry_lin`, make-leg/null arm
`resume_carry_rebase` (beside `resume_frame_restore` / `resume_coupling_restore`), both recorded in
the meta files.  Unit tests: a two-block driver round-trip with a shifted `x_lin` restores the
identical absolute DD; a chord missing an arc is dropped and counted; a blob without `ZL` restores
today's ẑ untouched.

**Gates (frozen now; graded on a NEW 6 h field seed written by the fixed exe with the write knob —
the new field harness seed `runs/evseed_field6h_v2` — against its own traces and against the
no-blob leg L1′ from the same seed):**

* **P1 byte-identity:** the fixed exe with both knobs OFF reproduces the seed-writer path and the
  L0/L1 legs byte-for-byte on `products.txt` (and the snapshot bytes without the `ZL` rows); write
  knob ON: products and every trace byte-identical to OFF, snapshot differs only by `ZL` rows.
* **P2 the seam is closed, from +1 on (not +5):** D5 flags at +1 ≤ 10 (was 676); `dd_fold` TOTAL
  norm at +1 within 10× of the same leg's +10 value (was ≥ 1e5×); emitted ≥ 58/61; |a₀₀ − seed|
  ≤ 2 TECU, `clock_std` ≤ 1.5× seed, `sigma0_pf_code` ≤ 1.5× seed; G5's wire/ZWD bars from +1.
* **P3 the branch is worth restoring (rebased leg vs L1′ no-blob leg, same seed):** chord births
  over +1…+5 ≤ 20 % of L1′'s; the satellite-clock contrast and code-bias |Δ| vs the seed at +1…+5
  ≤ L1′'s; silent PB steps at the seam ≤ 1 % of keys; consumer p50 over +5…+60 ≤ L1′'s.
  P3 failing with P2 passing = the branch restore is correct but worthless on this lineage — then
  the harness posture stays `state_snapshot_carry: false` and the ledger row says so.

**Sequel (not this card):** with a field seed that warm-resumes, the §20.1 wrong-narrowing price
(gates G-C/G-D, dark in §20) becomes measurable, and the reference-arc clause + the scope/pairs
field pin follow on that seed.
### 21.5 RESULTS — Phase 1 (the blob is the seam), the Phase 2 build merged, and the P2/P3 bars re-calibrated BEFORE the Phase 2 legs are read (2026-09-12 ~10:00 local; worker on worktree-agent-a2e7ed83282f533a8, manager re-grade with `scripts/ev_seam_grade.py`)

**Exe** `gnss_processor_rs2.exe` = main `cf08909` (sha256 `a35d2254…`; the worker's worktree was 147 commits
behind and was fast-forwarded first — the stale-branch-exe trap, caught).  Legs `runs/evleg_rs_null_{ctl,
nocarry,plain_nocarry,shgateoff}`, 60 epochs from ep0604, all `exit 0` with the same RTB-7 restore line
(256 D2 / 0 ambiguity / 61 Z / 1 pivot baseline; n_emitted 604).  Grader: `python scripts/ev_seam_grade.py
<legdir> --seed runs/evseed_field6h` (absolute paths — it resolves relative ones against its own repo).

| gate | L0 ctl (blob) | **L1 no-blob** | L2 plain, no-blob | L4 blob, SH gate off |
|---|---|---|---|---|
| G1 control | reproduces §21.1: 60/60 withheld, `dd_fold` 7.823e7, D5 676 — a₀₀(+1) **−4810.41** on `network_vtec_sh.csv` vs **−11109.10** on the SH gate's own log line (§21.1 quoted the gate; the grader prints both) | — | — | same first epoch as L0 |
| G2 availability | FAIL 0/60 | **PASS 60/60** | PASS 60/60 | PASS 60/60 (gate off) |
| G3 frame, from +5 | FAIL: \|Δa₀₀\| 1095.7 TECU, clock_std ×330.8, σ̂₀_code ×397.5 | **PASS: 2.25 TECU, ×0.876, ×1.053** | PASS: 1.64, ×0.804, ×0.880 | FAIL (as L0) |
| G4 births +5..+60 | 321 vs seed 296 (1.08×); D5 at +1 = 676 | **262 (0.89×); D5 at +1 = 0** | 302 (1.02×); 0 | 321; 676 |
| G5 wire + ZWD | NA | FAIL — see the calibration below | FAIL | FAIL: clock contrast p50 763 m, code bias 385 m |
| G7 a₀₀(+60) vs seed 37.47 | −471.49 | **36.56 (Δ −0.91)** | 38.58 (+1.11) | **−471.49 (Δ −508.96)** |
| consumer p50 from +5 (report) | no rows | 5.07 m (seed 8.83) | 10.51 m | 3487 m |

**What the four legs establish.**
1. **The blob is the seam defect** (L0 vs L1): dropping only the invariant DD carry restore turns 0/60 into
   60/60, a₀₀ tracks the seed to ≤ 2.25 TECU from +5 on, clock scatter and σ̂₀ sit at the seed's, D5 flags at
   +1 go 676 → 0, and the branch rebuilds cold (701 births at +1 — exactly the chords it declined to restore).
2. **The era is the carried memory's, not the gate's** (L4): with the SH gate off and the blob restored the
   wire emits garbage to +60 (clock contrast p50 763 m, consumer 3487 m, a₀₀ −471 at +60).  The withhold is
   protective — it is the only thing between the consumer and 61 epochs of poisoned product.
3. **The frame restore owns the PB-continuity half, not availability** (L2 vs L1): G2/G3 identical without
   it, but the PB seam step doubles (median ΔPB +5.41 m vs +1.79 m) and the consumer is worse (10.5 vs 5.1 m).

**G5 read correctly — two of its sub-bars were mis-calibrated, one is a real finding.**
* The clock sub-bar (p50 0.05 / p95 0.20 m) is TIGHTER than the seed's own epoch-to-epoch datum-invariant
  clock contrast (p50 **0.0907** / p95 **0.4034** m): no leg can pass it on this lineage.  The code-bias bar
  (0.02 m vs own step 0.0097) and the 5 cm PB threshold (own step 0.0025) are calibrated.
* `network_zwd.csv`'s `zwd_m` is not a physical ZWD on the field golden — the seed alone spans −30.5 … +13.5 m
  at one station over 6 h and −24.5 … +23.8 m across stations at one settled epoch (the field ZWD sink,
  `project_zwd_sink_census`): the 5 mm / 20 mm bars are sim-calibrated and mean nothing here.
* The real finding: **the no-blob resume re-realizes the phase wire ONCE, declared, and then does not converge
  to a well-defined wire.**  Each emitting leg writes exactly one `R` record at +1 naming 31 satellites
  (invariant #7 satisfied); the offset persists (per-satellite-median-removed |ΔPB| vs the seed p50 **9.4 m**
  at +2 — a genuinely different branch per (sat, signal), not a datum shift); the two no-blob legs disagree with
  EACH OTHER by 8.4 m p50; and the leg−seed clock contrast GROWS 0.89 → 3.41 m over the 60 epochs while each
  leg's own epoch-to-epoch continuity matches the seed's (L1 0.12/0.92 m).  The literal §21.3 clause reads
  FAIL because that declared step persists past +5; a cold branch is not a branch restore.

**Decision (per §21.3).**  L1 passes G2–G4 and G5's calibrated halves; the interim harness posture for field
IDENTITY legs is `state_snapshot_carry: false` in `--extra` (shared / Z / frame warm, DD branch cold), used by
§22's legs.  It is NOT a posture for grading absolute wire continuity across the seam — that is Phase 2's job.

**Phase 2 — built and MERGED (main `2c62724`; worker commits `62303d2` + grader `b903b66`).**  As pre-registered
in §21.4 with three deviations the worker flagged and the manager accepted: (i) `lin_of` in
`NetworkDdPriorBuild.cpp` lives inside the lane machinery (`mw_mach_on`, off on both goldens) — the arc's
`x_lin` is taken from the station state vector on the path that runs (`ArcInput::x_lin/x_lin2`, read only under
the two knobs); (ii) the writer's spanning tree is not serialized, so the re-base cannot recompute the writer's
cycles per chord — it rides the cycle-basis projection `W` the build already forms, `ẑ_new = W·ẑ_old +
(W·zl_writer − zl_now)`, which is frame-blind (`Z_new = W·Z_old` in arc space) and reduces to the §21.4 rule
exactly when the tree is unchanged; applied to the EMITTED block, never to `carried_`, so a RAIM rebuild is
idempotent; (iii) "a chord with an absent arc is dropped" is the existing birth path — `n_resume_carry_dropped_
unbased` counts the case with no exact re-base (a restored contributor with no `ZL` row).  Knobs
`network.state_snapshot_carry_lin` (write; disarmed without `state_snapshot_carry`) and
`network.resume_carry_rebase` (read; forward-only resume policy, NOT fingerprinted); one `ZL` row per block
after `Z`; run-end `Network (RS-2 linearization carry): …`; a "Linearization frame: …" token on the RTB-7c
restore line; unit suite `ResumeCarryRebaseTest` (4: per-arc-shift round trip with the identity control,
un-basable ⇒ diffuse birth + counted, legacy blob untouched, write-off ⇒ no `ZL`).  Debug suite 1968 / 0 fail.
**P1 on the worktree build: PASS** — 20-epoch field-golden smoke, `products.txt` `b9da8cc1…` in both arms,
the snapshot differs only by the four `ZL` rows (+ the write timestamp), and the warm-resume fingerprint is
**unchanged, `545b8bdd2b638231` = the existing seed's** (a `ZL`-bearing snapshot is chain-compatible).
Main's own rebuild must reproduce the same hashes before the Phase 2 legs count (owed to the worker now).

**P2 / P3 bars, RE-CALIBRATED and frozen now (the Phase 2 seed `runs/evseed_field6h_v2` is being written;
legs `rs2_null_{rebase,norebase,nocarry}` from its ep0604, one exe).**  The yardstick is the seed's OWN
epoch-to-epoch step over the leg window, computed by the grader for each channel (own-step p50 / p95):
* **P2a seam closed, from +1 on (not +5):** emitted ≥ 58/61; |a₀₀ − seed| ≤ 2 TECU; `clock_std` ≤ 1.5× and
  `sigma0_pf_code` ≤ 1.5× the seed's at every epoch; D5 flags at +1 ≤ 10 (was 676); `dd_fold` TOTAL at +1
  within 10× of the same leg's +10 value (the recorder must be forced on if it does not trip — otherwise this
  sub-bar is read from the D5 count alone).
* **P2b wire, leg − seed at matched tows, pooled +2..+60:** clock contrast p50 ≤ 3× own-step p50 and p95 ≤
  3× own-step p95; W6 code bias p50 ≤ 3× own-step p50; ZWD graded by the same own-step rule, not the mm bars.
* **P2c phase wire at the seam (the discriminator):** the `R` record at +1 names ≤ 3 satellites (no-blob: 31);
  per-satellite-median-removed |ΔPB| vs the seed at +2 p50 ≤ 0.05 m (no-blob: 9.4 m); no DRIFT after +2:
  |Δ(k) − Δ(+2)| ≤ 0.05 m on ≥ 90 % of PB keys at every epoch to +60.
* **P3 the branch is worth restoring (rebased vs `rs2_null_nocarry`, same seed):** births over +1..+5 ≤ 20 %
  of the no-blob leg's; leg−seed clock contrast p50 at +60 ≤ the no-blob leg's at +5 (no growth); consumer p50
  over +5..+60 ≤ the no-blob leg's.  `rs2_null_norebase` (blob, re-base off) must reproduce the L0 failure on
  the new seed — the control that proves the fix is the re-base and not the seed.
P2a passing with P2c failing ⇒ the frame is right but the branch content is not (a second-order seam);
P3 failing with P2 passing ⇒ correct but worthless on this lineage — then the harness stays no-blob and the
ledger row says so.

### 21.6 RESULTS — Phase 2: the re-base is exact and total, and it is NECESSARY BUT NOT SUFFICIENT; RS-2b pre-registered (2026-09-12 ~12:30 local; seed `runs/evseed_field6h_v2`, exe `gnss_processor_rs2seed.exe` 0fe4b64e… = main 2c62724's tree; legs `runs/evleg_rs2_null_{rebase,norebase,nocarry}` from ep0604, one exe; manager re-grade with `scripts/ev_seam_grade.py`)

**P1 on main's own rebuild: PASS** — `gnss_processor_rs2main.exe` (520c1106…, main after the RS-2 merge)
reproduces `products.txt` `b9da8cc1…` in BOTH smoke arms; the snapshot differs only by the four `ZL` rows;
fingerprint `545b8bdd2b638231` in both.  **The v2 seed:** 723 synced / 720 emitted / 0 withheld, 12 snapshots,
ep0604 at the same tow as v1's (277290) with 4 `ZL` rows and the SAME fingerprint — the write knob is
non-fingerprinted on a real 6 h lineage.  The only substantive config delta from v1 is
`cycle_slip.mw_evidence_refloat: true` (the §20.1 field pin, promoted after v1 was written), so v1 and v2
legs are not cross-comparable; every comparison below is within v2.

| | **rebase** (blob + re-base) | **norebase** (blob, re-base off) | **nocarry** (no blob) |
|---|---|---|---|
| disposition | **61/61 emitted** | **61/61 `sh_gate_withheld`** | 61/61 emitted |
| G3 from +5: worst \|Δa₀₀\|; clock_std ×; σ̂₀_code × | 2.73 TECU @+7; 0.902; 1.126 | 1095.7; 330.8; 397.5 | 2.25 @+43; 0.876; 1.053 |
| a₀₀(+60) − seed | −0.61 | −508.96 | −0.91 |
| rhs-attrib at +1 | no trip | `dd_fold` 7.823e7 (v1's table to 4 s.f.) | no trip |
| RS-2 run-end | write ARMED / read ARMED; ZL on 4 blocks; **re-based 637 rows**, max \|Δẑ\| **411 715 m**, 0 un-basable | read off; re-based 0 | no blob |
| chord births +1 / +2 / +3 / +4 / +5 | 64 / **305** / 4 / 8 / 0 | 64 / 674 / 8 / 16 / 6 | 701 / 7 / 5 / 8 / 0 |
| D5 flags / re-taps at +1; +2 | **301 / 301**; 0 / 0 | 676 / 676; 4 / 4 | 0; 1 |
| R records | +1: 30 sats (the only one) | none (nothing emitted) | +1: 30 sats (the only one) |
| per-sat-median-removed \|ΔPB\| p50 at +2 / +60 | 14.05 / 13.79 m | NA | 9.98 / 9.65 m |
| clock contrast p50/p95 at +5; at +60 | 2.95/7.49; 4.00/10.92 m | NA | 1.68/6.91; 3.41/9.03 m |
| pooled +5..+60: clock p50/p95; CB p50; ZWD p50/p95 | 3.64/11.75; 0.669; 2.14/9.43 | NA | 2.50/10.49; 0.675; 4.12/14.36 |
| consumer p50 from +5 (seed 8.94) | 5.18 m | no rows | 5.05 m |

Seed yardsticks (own epoch-to-epoch step, tows 277320–279090): clock contrast 0.0906 / 0.4030, W6 code bias
0.0097 / 0.0390, phase bias 0.0027 / 0.0076, station ZWD 0.107 / 0.569 m (p50 / p95) — within 2 % of v1's.

**What is established.**
1. **The re-base is the fix for the catastrophe, single-knob and total:** same seed, same exe,
   `resume_carry_rebase` alone takes the resume from 0/61 to 61/61 with a₀₀, clock scatter and σ̂₀ at the
   seed's.  `norebase` reproduces §21.1 to four significant figures on a freshly written seed.
2. **The wiring is complete, not silently partial:** 637 re-based + 64 not carried = 701 restored chords; the
   64 are exactly the chords that fail to carry at +1 in BOTH blob-restoring legs (arc churn at the seam); 0
   un-basable.  The correction scale — max |Δẑ| 412 km — is the code-seeded linearization's distance from the
   writer's converged one, the same order as the 256 km cold-start leak `cold_clock_seed` addresses.
3. **P2a / P2c / P3 FAIL as pre-registered.**  D5 flags at +1 are 301, not ≤ 10 (676 halved, not removed);
   305 chords are reborn at +2; the emitter still re-realizes 30 satellites at +1; per-sat-median-removed
   |ΔPB| at +2 is 14.05 m, not ≤ 0.05; and on every pooled wire channel except ZWD the re-based leg ends
   FURTHER from the seed than the no-blob leg (clock 3.64 vs 2.50 m, PB 13.8 vs 9.7 m), the two emitting legs
   differing from each other by 1.5 m (clock) and 4–5 m (PB).  The interim harness posture stays
   `state_snapshot_carry: false`.
4. **The residual is a first-epoch DISAGREEMENT between the restored branch and the cold station layer, not a
   frame error the re-base missed.**  The 301 flagged chords (`network_d5_flags.csv` at +1): |v| p10 4.8 /
   p50 11.0 / p90 27.1 / max 104 m against a modelled `var_v` p50 0.33 m² (σ 0.58 m) — a 19σ disagreement, on
   L2W (171) AND L1C (113) AND L1W (17), across 51 stations.  For comparison the seed's own 52 D5 flags in the
   same window have |v| p50 91 660 m with diffuse `var_v`: they are the born-chord artefact the D5 v5 note
   describes (`carry_invariant_innov_carried_only` is NOT armed on the field golden — hygiene item), so the
   leg's +1 flags are the first carried-chord innovations this lineage has produced at a seam.  Two readings
   remain open and they are opposite: (a) the branch is right and the single-epoch data-only DD is wrong by
   metres because the station layer's memory (rx-CB carry, the T5 iono deviation, the PB realization) is not
   in the snapshot — then holding the branch through the first epochs lets the station layer converge around
   it; (b) the branch is inconsistent with this run's data for a deeper reason (a shared-border frame the
   local re-base cannot reach) — then holding it degrades the solve.  §21.5's "P2a passing with P2c failing ⇒
   second-order seam" is the case we are in; the D5 gate erases the branch before either reading can be told
   apart, which is why the next increment is a measurement, not a fix.

**RS-2b pre-registered — the D5 resume warm-up (`network.resume_d5_warmup_ep`, int, default 0).**  For the
first K epochs after a WARM resume the D5 innovation gate is diagnostic-only: trips are counted
(`d5_warmup_suppressed`, and the trace row keeps its columns plus a `warmup_suppressed` flag) but neither
re-tapped nor mark the arc slipped; the MW / LLI / IF-TD drains stay LIVE (a real slip still refloats), the
corroboration gate is untouched, cold starts are unaffected (the countdown is set only at a warm resume).
Forward-only resume response policy, fingerprint-EXEMPT (the `resume_slip_warmup_ep` class).  Legs from the
v2 seed ep0604, blob + re-base, `--extra` `resume_d5_warmup_ep` 5 (`rs2_null_rebase_d5w5`) and 1
(`rs2_null_rebase_d5w1`, the dose control):

* **P4a the branch survives:** births at +2 ≤ 20 (was 305); D5 suppressed count reported per epoch +1..+K;
  D5 flags at +K+1..+K+5 (the first live epochs) ≤ 10 per epoch — if they return at hundreds the branch was
  wrong (reading b).
* **P4b the solve stays sane with the branch held:** emitted 60/60; |a₀₀ − seed| ≤ 2 TECU at every epoch from
  +1; clock_std and σ̂₀_code ≤ 1.5× the seed's; no SH-gate or σ̂₀ withhold.
* **P4c the branch carries information (reading a):** pooled +5..+60 clock contrast p50 ≤ the no-blob leg's
  2.50 m AND ≤ 3× the seed's own step (0.27 m) for a full pass; per-sat-median-removed |ΔPB| at +2 ≤ 1 m
  (no-blob 9.98); consumer p50 ≤ 5.05 m.  The R record at +1 is recorded — 30 satellites again would mean
  the PB re-realization at a warm resume is structural to the emitter on this lineage (a separate item),
  not a branch property.
* P4a+P4b pass with P4c failing ⇒ the branch is right but the wire's PB/clock realization is set elsewhere
  (the station layer / emitter memory: RS-3, snapshot the rx-CB carry, the T5 deviation and the PB
  realization).  P4b failing ⇒ reading (b): the restore is unfaithful beyond the local frame; the harness
  stays no-blob and the ledger row says so.

**Ledger (this commit, `scripts/_rs2_refarc_ledger_oneoff.py rs2 PIN …`):** `network.state_snapshot_carry_lin`
RECOMMEND_SCOPED, PINNED on the field golden (write side, products byte-identical — every snapshot-writing
run of this lineage now carries its frame); `network.resume_carry_rebase` RECOMMEND_SCOPED with the
necessary-not-sufficient record above, armed by the harness on every resume leg, pinned nowhere.

### 21.7 RESULTS — RS-2b: the branch was right; the wire's realization is set by two datum memories the snapshot never carried; RS-3 pre-registered (2026-09-12 ~13:45 local; legs `runs/evleg_rs2_null_rebase_d5w{5,1}`, seed v2 ep0604, exe `gnss_processor_rs2b.exe` a31248d0…; manager re-grade)

**Build (merged `b6afe8c`; registered `58c4088`).**  `network.resume_d5_warmup_ep` (int, default 0): armed only in
`networkTryWarmResume`, counted down at the install seam (the D5 gate's single site), trips counted
(`d5_warmup_suppressed`, a `warmup_suppressed` column on `network_d5_flags.csv`) but neither re-tapped nor
funnelled; σ̂₀ feed, corroboration gate and the MW/LLI/IF-TD drains untouched; not fingerprinted.  Cold smoke
with K=5 hashes `b9da8cc1…` (inert without a resume); Debug suite 1969 / 0 failed; one log-order defect the
worker found in its own retire line was fixed before the legs were re-run (`c96182a`).

| | base (blob + re-base) | **K = 5** | **K = 1** |
|---|---|---|---|
| D5 flags / re-taps / suppressed at +1 | 301 / 301 / 0 | 301 / **0** / **301** | 301 / **0** / **301** |
| chord births +1 / +2 / +3 / +4 / +5 | 64 / **305** / 4 / 8 / 0 | 64 / **7** / 4 / 8 / 0 | 64 / **7** / 5 / 10 / 2 |
| live D5 trips after the window | (+7: 1) | +6..+10: 0, **1**, 0, 0, 0 | +2..+6: **1, 2, 2**, 0, 0 |
| D5 rows over the leg / run re-taps | 332 / 332 | 332 / 27 | 332 / 31 |
| G2 / G3 (worst \|Δa₀₀\| from +5; clock_std ×; σ̂₀ ×) | PASS / 2.73; 0.902; 1.126 | PASS / 2.56 @+9; 0.903; 1.178 | PASS / 2.56 @+9; 0.903; 1.179 |
| Δa₀₀ at +1 … +5 (all legs alike) | +4.95, +2.50, +2.83, +2.41, +2.48 | +4.95, +2.27, +2.82, +2.26, +2.34 | +4.95, +2.27, +2.92, +2.55, +2.31 |
| R record at +1 | 30 sats | **30 sats** | **30 sats** |
| per-sat-median-removed \|ΔPB\| p50 at +2 / +60 | 14.05 / 13.79 m | 14.35 / 13.64 | 14.35 / 13.78 |
| clock contrast pooled +5..+60 p50 / p95 | 3.64 / 11.75 m | 3.59 / 11.91 | 3.61 / 11.94 |
| W6 code bias pooled p50 | 0.669 | 0.651 | 0.656 |
| ZWD pooled p50 / p95 | 2.14 / 9.43 | **1.66 / 8.58** | **1.65 / 8.62** |
| consumer p50 from +5 (seed 8.94) | 5.18 | 8.91 | 5.24 |

**P4a PASS, P4b PASS, P4c FAIL — the pre-registered reading (a): the branch is right, the wire's realization
is set elsewhere.**  Holding the branch through ONE epoch removes the +2 birth wave (305 → 7); the 301 trips
at +1 are identical in every arm and never return — the gate then runs at the seed's own rate (0–2 per
epoch, the same 332 D5 rows everywhere).  So the +1 disagreement was one epoch of cold station layer, not a
branch inconsistency, and K = 1 suffices.  The Δa₀₀ sub-bar (≤ 2 TECU from +1) is failed identically by
every resume posture including the no-blob one (+2.3 … +2.9 TECU over +2..+10, decaying to < 1 by +60) — a
common ~30-min SH-level transient at the seam, recorded, not a branch property; P4b is judged on the G3
5-TECU bar.  P4c: the wire does not move at all — clock contrast, code bias and the per-satellite PB offset
are unchanged to within 3 %, the emitter still declares 30 satellites at +1, and only ZWD is quieter (23 %).
The consumer split (K=5 8.91 vs K=1 5.24 m on wires that differ by < 1 %) is the in-process consumer's own
state, not the wire — §18.14 stands: grade seams with a cold-consumer replay.

**Decision.**  `network.resume_d5_warmup_ep` RECOMMEND_SCOPED at K = 1 (`58c4088`); `scripts/event_leg.py`
arms it on every resume leg beside `resume_carry_rebase`, so the harness posture for field legs is now
**blob + re-base + one held epoch** (branch continuity is what the slip cards grade; the datum re-draw is
common to every posture and cancels in event − null).  The no-blob posture remains available by `--extra`.

**The two datum memories (read from the code, confirmed by the leg's own events).**  At +1 the leg writes
**110 `PHASE_DATUM / eq34_reref` events** (the seed: 2 per epoch) and **30 `CB_DATUM / reseed_disc_landed`**
(the seed: 0):
1. The eq34 chain datum's spanning tree is re-elected from scratch at the seam.  The election
   (`NetworkChainDatum.cpp`, STICKY-tree seed at `:230–270`) continues last epoch's tree through
   `eq34_datum_rx` (the sticky datum receiver per (constellation, signal)), `eq34_rx_parent` (each
   receiver's parent-edge satellite), `eq34_sat_root` and `eq34_prev_target` — documented at
   `NetworkRunContext.hpp:794–815` and NEVER written to the snapshot — plus `anchor_incumbent` (the sat-side
   parents, which the frame restore DOES bring back: 126).  With the receiver-side maps empty the tree is
   rebuilt from the cold arc registry, the pinned arcs change, and every phase-bias family re-realizes by the
   integer between the old and new pinned arcs: 110 rerefs, the R record naming 30 satellites, per-satellite
   PB offsets of 14 m.  The DD branch cannot prevent this — it is invariant to the datum by construction.
2. The time-linked code-bias datum's previous-value map (`code_bias_prev_` / `code_bias_sat_seen_`,
   `NetworkRunContext.hpp:1128–1142`) is "NOT persisted", and the resume block sets
   `code_bias_datum_resumed_ = true` so that every first post-resume re-seed disc-bumps
   (`NetworkRunContext.cpp:1655–1665`, landed at `NetworkSolPrep.cpp:~880–900`): the W6 code-bias / clock
   split is re-drawn for 30 satellites — the 3.6 m clock-contrast and 0.65 m code-bias offsets.

**RS-3 pre-registered — datum continuity across the seam.**  Write knob `network.state_snapshot_datum_capture`
(default false; beside `state_snapshot_frame_capture`): the snapshot carries the eq34 tree maps
(`eq34_datum_rx`, `eq34_rx_parent`, `eq34_sat_root`, `eq34_prev_target`, `eq34_prev_el`,
`eq34_retained_edges`) and the time-linked code-bias datum memory (`code_bias_prev_`,
`code_bias_sat_seen_`) as format-forward rows; write side only, products byte-identical.  Read knob
`network.resume_datum_restore` (default false; forward-only resume response policy, fingerprint-EXEMPT;
requires `resume_frame_restore`): restore the maps before the first election so the sticky-tree seed
continues the writer's tree, and restore the code-bias memory so `code_bias_datum_resumed_` stays false
and a re-seed bumps only where it genuinely re-seeds; rows absent ⇒ today's behaviour, loudly.  Counters and
a run-end line (`eq34 edges / datum rx / prev targets restored; CB prev values / seen restored; first-epoch
eq34 rerefs, reattaches, CB re-seed discs`).  New seed `runs/evseed_field6h_v3` (write knob via `--extra`),
legs from its ep0604 on the harness posture, one exe: `rs3_null_restore`, `rs3_null_norestore` (the
control — must reproduce K = 1's 110 / 30 / 30-sat / 14 m), `rs3_null_nocarry_restore` (the datum half
without the branch).
* **P5a the datums continue:** `eq34_reref` at +1 ≤ 5 (was 110); `reseed_disc_landed` at +1 ≤ 3 (was 30);
  the R record at +1 names ≤ 3 satellites (was 30).
* **P5b the wire is continuous, pooled +5..+60 and at +2:** per-sat-median-removed |ΔPB| at +2 ≤ 0.05 m
  (was 14.35); clock contrast p50 ≤ 3× the seed's own step (0.27 m; was 3.6) and p95 ≤ 3× (1.2 m); W6 code
  bias p50 ≤ 0.03 m (was 0.65); ZWD p50 ≤ 3× own step (0.32 m; was 1.65); no silent PB step > 0.05 m on
  > 10 % of keys at any epoch from +2 on.
* **P5c availability and branch unchanged:** 60/60 emitted; a₀₀ on the G3 bar; births +2 ≤ 20; D5 rows ≈ 332.
* **Consumer:** a cold-consumer replay of the leg's wire against a replay of the seed's wire over the same
  window (`network.closed_loop.replay_products_path`, §18.14), reported; the in-process consumer is not graded.
* P5a passing with P5b failing names a THIRD memory (the rx-CB carry, the T5 iono deviation) — then the
  first-epoch offsets per channel say which; P5a failing means the sticky seed does not engage on restored
  maps under `carry_invariant` (its carry-value test), and the fix moves into the election.

### 21.8 MAIN CERTIFICATION after the day's merges (2026-09-12 ~14:20 local; worker on main's own tree)

main `58c4088` (RS-2 `2c62724`, reference-arc clause `4b88ca2`, RS-2b `b6afe8c`, the three ledger/posture
passes): `src`/`include`/`scripts` clean; Debug and Release rebuilt with the wrapper, first attempt.  **Full
Debug suite on main: 1975 cases / 269 suites, 1974 passed, 1 skipped (the documented fixture gate
`FixtureSmokeTest.Sp3HeaderTimeConversionMatches`), 0 failed** (the 17 skips seen in worktrees are the
`data/`-fixture gates that resolve on main's baked data root).  Smoke pair on `gnss_processor_main_close.exe`
(74f9ed0b…): `products.txt` **`b9da8cc1…` in both arms** (the golden as it ships with `state_snapshot_carry_lin`
pinned vs the same with it false), the snapshot differing only by the four `ZL` rows, fingerprint
`545b8bdd2b638231` both; the POSTURE block lists `state_snapshot_carry_lin` (armed ⇒ no advisory in the ON arm,
`POSTURE OFF [SCOPED]` in the OFF arm), `resume_carry_rebase` and `resume_d5_warmup_ep` (off on a seed
writer, correctly), and omits `common_band_ref_arc` because its parent scope is the field exception (out of
regime, row present at `GnssConfigNetworkPosture.cpp:201`); zero unknown-knob warnings.  **Closing null leg
`runs/evleg_close_null_main`** from `evseed_field6h_v2` ep0604 with `event_leg.py`'s registered defaults
(frame restore, re-base, `resume_d5_warmup_ep` 1, coupling restore false): 61/61 emitted, 301 suppressed /
637 re-based, and `products.txt` `d75d12a6…` **byte-identical to `rs2_null_rebase_d5w1`'s** on a different
binary — the harness posture reproduces the hand-armed leg exactly.

