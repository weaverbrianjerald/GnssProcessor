# SESSION LOG — overnight autonomous campaign, 2026-08-07 (Fable)

Running log, updated incrementally as gates land (user directive: document major
findings as work proceeds, in case of usage limits).  Polished results get folded
into `SPEC_level_information_budget.md` (§28+) and `HANDOFF_edge_response_queue.md`
when each gate closes; this file is the chronological record.

## Charter (user, 2026-08-06 evening)

- Find and correct network processing issues.  SPEC files are orientation, not gospel.
- Run budget: **≤ ~10 min wall per run** (63 sta → ≤ ~1 h data; 305 sta → ≤ ~15 min data).
- The EXT-CB "all external satellite info constrained" test: continue **or not** — my call.
  Concern on record: network errors invisible because pushed into other estimables.
  (Note: SPEC §17 item 0 already measured exactly this — DD fractions identical ⇒
  estimability-neutral frame shuffling; consumer replay NOT equivalent ⇒ the real
  damage is product internal consistency, not the level.  That analysis stands.)
- Ultracode authorized.  Overnight autonomous.

## Plan of record (set ~22:00)

1. **Item 3 BUILD — S-consistent harvest** (SPEC §17 item 3 / §27 item 1, ranked #1):
   per-epoch post-solve Λ-weighted de-mean of the HARVESTED rx-CB realization
   (solve → transform → harvest).  Default-OFF, OFF-path byte-identity, unit tests,
   LASER oracle (`C:\LASER\test\GPS3710_test_PMINOLESS`).  Staged: 20-ep rank →
   1 h stability → 3 h level read vs arm 32.  12 h verdict stays DEFERRED (>10 min).
2. **H-rider pair** (§27 item 2c): (a) batch replay reader drops all H records;
   (b) H σ₀ ≡ 0 (feed is carry_warm-gated within-epoch RAIM; candidate = prefit census σ̂₀).
3. **Cheap analyses on existing artifacts**: casualties 1+2 on `runs/ext63_1h_evt_pl`
   (within-epoch RAIM alive? H σ₀ alive?); Q4 power-fixed replay (consumer station
   chosen from G16/G11 visibility).
4. **Stretch — Edge 2**: first validation of `clock_datum_sum_to_zero` /
   `_broadcast_anchored` on the leveled 1 h lineage.

Constraints honored: Release-only data runs on a RENAMED, HASHED exe; runs strictly
serialized (shared spdlog dir); pair on build AND resolved config chain; new work
default-OFF byte-identical; no absolute level verdicts off short windows (rank/residual
only); no co-author trailer (hooksPath verified `.githooks`).

## Tree state at start

- Uncommitted (from prior/concurrent sessions, NOT mine): piece-2 knob
  `network.mark_refsat_reset_disc` (NetworkSlipQc/NetworkRunContext/NetworkSolPrep/
  ApplicationNetworkAssembly/GnssConfig*, gated §1.8–§1.9), sim/ iono-disturbance
  workstream, pyviz/ exam-viewer workstream.  Leaving all in place; compound commit
  convention at the end.
- Baseline Debug `gnss_tests` build+run: launched, result pending (logged below).

## Findings

### F0. Baseline (2026-08-07 start)

Debug `gnss_tests` rebuilt against the uncommitted tree: **1274 passed / 1 skipped
(fixture-gated) / 0 failed** — 1275 cases, up from 1264 at the Aug-5 hand-back
(the piece-2 session added coverage).  Tree is a valid base for new work.

### F1. Orientation findings that pre-answer task 3 (free, from existing artifacts)

- **Casualty (1) — within-epoch RAIM on the leveled arm: ALIVE.**  In
  `runs/ext63_1h_evt_pl/logs/network_flag_rates.csv`, tier T2 shows **115
  opportunities / 0 flags** — live on exactly the 115 carry_warm epochs (gate
  opened at epoch 7 per `phase_leveling_warmup_epochs: 6`), quiet hour so no
  trigger.  T3/T3c/T4: 0 opportunities.
- **Casualty (2) — H σ₀ on the leveled arm: DEGENERATE, not dead.**  121 H
  records: 6 × `0.0000` (warm-up) then 115 × a constant `1.0000` (`n_adapt = 0`
  every epoch — the vacuous-pool publication, not a live σ̂₀).  So leveling
  un-zeroes the field but the wire still carries no usable statistic —
  confirming the census feed as the right fix (implemented tonight, below).
- **Q4 non-detection EXPLAINED — the experiment was unpowered by geometry.**
  G16@TOW 259470 is visible from the Pacific (MAJU/KOKV/MIZU...), G11@262020
  from the South Atlantic (STHL/WIND/PALM...); **intersection EMPTY**, and the
  held-out consumer WTZZ00DEU sees neither.  Slip origins: G16 ← KOKV00USA,
  G11 ← STHL00GBR (both also near-highest-elevation observers).  Power fix =
  two single-event replays with non-origin high-elevation consumers:
  **MAJU00MHL** (G16) and **WIND00NAM** (G11).
- Posture audit: every `_ext63*` chain resolves through the same 7-deep spine;
  postures confirmed per the denominators rule.  Current `gnss_processor.exe`
  = the edge2 family (`5E749970…`) — every A/B tonight re-runs its control on
  MY new binary.
- ⚠ `_ext63_1h_evt_pl.json` lacks `state_snapshot_require` (posture checker:
  a fingerprint mismatch would SILENTLY cold-start on a resumed leg) — noted
  for any future warm-resume use; tonight's arms are all fresh-convergence.

### F2. IMPLEMENTED — Card S2H, the "S-consistent harvest" (§17 item 3 / §27 item 1)

Knob `network.rx_cb_harvest_demean` (+ `network.rx_cb_harvest_screen_z`, default
8.0), default OFF, fingerprinted (`rxcbharv:`), forces the superseded LV-2/LV-5
fold-side de-mean OFF with a warn.  Mechanism: at the serial post-solve harvest,
AFTER the LV-4 erase pass and the gap prune (member set = exactly the
fold-eligible set), per family (CB state key), subtract the Λ-weighted mean of
the harvested ABSOLUTE realizations from every carried entry (λ = 1/var); exact
diagonal covariance co-transform (voters var − 1/Σλ, screened var + 1/Σλ =
diag(S·Q·Sᵀ), verified vs brute force in tests).  Robust peer screen on the μ
vote (median/MAD, floor = median formal σ, z = 8) per SPEC §16/12.1; thin
epochs apply NOTHING (identity, counted — never a frozen μ).  Trace: one
`s2h_mu` events row per family per epoch; armed-only run-end line.

Two documented deviations from the LASER original (both load-bearing):
(1) de-mean about the DATUM ROW's target (absolute zero), NOT about x_lin —
de-meaning about x_lin is LV-2's measured failure (pins mean(x_lin), SPEC
§13.2); (2) Λ-weighted, because the fold's common-mode force is ∝ λ — only the
weighted mean zeroes the NET force; λ ≡ const reduces exactly to the LASER
oracle case.

Touchpoints: NetworkRunTypes.hpp (`rxCbHarvestMu` pure helper),
NetworkRunContext.{hpp,cpp} (knobs, carry struct, fingerprint, the harvest
pass), GnssConfig.hpp, GnssConfigLoadNetwork.cpp (parse + guards),
NetworkLeveling.cpp (trace), NetworkTeardown.cpp (run-end line),
tests/test_rx_cb_harvest_demean.cpp (14 cases: Λ-mean identities, idempotence,
realization invariance, **LASER GPS3710 PMINOLESS golden oracle** (skip-gated
on C:\LASER), SQSᵀ closed form vs Eigen brute force, peer screen, thin/degenerate,
loader contract), CMakeLists.

### F3. IMPLEMENTED — the H-heartbeat rider (§27 item 2c / edge-queue §1.6 item 3)

**(a) Batch replay reads H.**  `NetworkProductReader::read` gained an optional
4th out-param collecting EVERY H in file order (incl. standalone WITHHELD-epoch
H's, which complete no record and were invisible to both the records and
`epoch_status`).  `ApplicationConsumerReplay` forwards them to
`SsrCorrectionStore::setProducerStatus` interleaved by content epoch, deduped —
the batch analogue of the RTA-6 tail wiring in ApplicationRealtime — plus an
invariant-#4 run-end count ("N on the wire, M forwarded").  Solve determinism
untouched (status channel is observability; `logProducerStatus` logs one-shot
transitions).

**(b) Census-fed H σ₀.**  Knob `network.h_sigma0_prefit_census` (default OFF;
requires `emit_epoch_status_record` + `trace_prefit_census`, INERT-warn
otherwise) + `network.h_sigma0_classes` (default "code,sh,other" — phase
excluded per census deviation 2c).  New `networkPrefitSigma0` in
NetworkPrefitCensus.cpp: same statistic/centres/taxonomy as INC-1
(common-mode-removed s = |y−m|√w), pooled across stations over the class mask,
σ̂₀ = 1.4826·median; born-cell deferral via the census's lag-1 presence map;
pin-regime rows excluded; empty pool → 0.0 = the H grammar's "unavailable"
sentinel.  Computed ONCE per epoch PRE-solve at the top of networkSolveGlue so
ALL FOUR H sites carry it — including the pre-solve gate withhold, whose σ₀
was structurally 0.  UNIFORM statistic per wire; the S contract advertises
`hsig=pc` (new optional token, byte-identical when absent, legend updated);
the A4 withhold ceiling still reads `sigma0_adapt`, unchanged.  Armed-only
run-end line.  Tests: tests/test_h_heartbeat_rider.cpp (7 cases: batch-read
surfacing incl. the standalone withheld H, 3-arg source-compat, hsig
round-trip + byte-absence, loader contract).

### F4. Unit suite GREEN with both cards: 1298 passed / 1 skipped (1299 cases, +24)

Including **the LASER GPS3710 PMINOLESS golden-oracle test** — the port recovers
LASER's published PMINOLESS realization from a re-pinned RLESS input on the real
golden CSV (per-point ≤ 5e-5 m), and the SQSᵀ covariance closed form matches an
Eigen brute force to 1e-12.  Session Release binary: `gnss_processor_s2h.exe`
md5 **82B4450703C551109DCEFB0B6C212559** (all tonight's producer A/Bs live on it;
Q4 replays stay on `gnss_processor_edge2.exe` = the original pair's binary).

### F5. Run matrix (staged, all ≤ 10 min wall, serial — predictions in the configs)

Batch A (running): 4× Q4 power-fix replays (MAJU/WIND, marks/nomarks, edge2 bin);
H-rider replay byte-identity gate (`_ext63_replay_1h_pl_marks_s2hbin`); OFF-path
byte-identity vs the §1.8 anchor md5 (`_ext63_1h_evt_off_s2hbin`); 20-ep smokes
ctl / s2h_on / hsig_on.
Batch B (gated on A): 3 h control (arm-32 posture, session binary) + 1 h S2H
stability arm.  Batch C (gated on B's 1 h gates): 3 h S2H arm.
12 h verdicts remain DEFERRED per the run budget.

### F6. Batch A results

**★ Q4 CLOSED — the R-record consumer link is PROVEN end-to-end (edge-queue §1.9
Q4, was "untested, underpowered").**  Power fix: two single-event replay pairs
with non-origin high-elevation observers as consumers (MAJU00MHL for G16,
WIND00NAM for G11; original WTZZ00DEU consumer observes NEITHER marked
satellite — observer-set intersection is empty).  Timestamped verdict:

| pair | mark | marks arm | nomarks arm |
|---|---|---|---|
| MAJU | `R,G16` @ TOW 259470 | **reset G16 L1C+L2W @ exactly 259470** | none at 259470 |
| WIND | `R,G11` @ TOW 262020 | **reset G11 L1C+L2W @ exactly 262020** | none at 262020 |

(Shared in both twins: the 259230 ignition resets and WIND's 262200 value-step
reset — the mark resets are ADDITIONAL and mark-caused; the two products are
byte-exact apart from the R rows.)  The producer chain §1.9 validated now has
its consumer half: IF-TD refsat_reset (carry_warm) → mark → `R` record →
`markSatDiscontinuity` → both-band consumer ambiguity reset at the event epoch.
Caveat for the record: consumers are IN-NETWORK (mechanism proof, not an
accuracy grade), and the VALUE of marking (net benefit vs reset transient —
WIND marks hour-0 3D max 38.9 m vs 12.8 nomarks is the transient's face) still
needs the onset-enrichment grade on a leveled long arm (§1.4 pattern).

**Gate 1 (OFF-path structural byte-identity across the rebuild): PASS (§1.7
sense).**  `ext63_1h_evt_off_s2hbin` vs the stored §1.8 anchor: **16,286 of
16,287 lines byte-identical — every E/C/B/P/H/S record**; the single diff is
the `#` legend comment line, which now documents the optional `hsig=` token.
121/123 epochs emitted, 0 gate-withheld, run 91 s.

**Gate H-a (replay solve-identity with H-forwarding live): PASS byte-identical.**
`replay_1h_pl_marks_s2hbin` on the session binary: `closed_loop_trace.csv` md5
`E12AE34433BC…` equals the stored original exactly — the status channel is pure
observability, solve untouched.

**20-ep smokes, first attempt: ALL THREE (incl. control) 0 epochs — MY config
error, and a cheap re-payment of the §1.5 config-chain lesson.**  The arm-32
posture lives partly in its LEAF: `estimation.global_vtec_sh` (a₀₀ anchor
32.24±2), `network.rx_code_bias_datum`, `network.sbasis_complete` are NOT in
the base chain.  Without anchor+datum the level ⊕ rx-DCB directions are
genuinely open and the observability gate correctly withholds every epoch
(CONTRACT §2.3 posture — "never grade that configuration"; gate trace:
identical inputs, gate_ok 0 vs 1).  All six S2H/HSIG configs patched with the
full arm-32 leaf posture; trio re-run below.
**Lesson worth keeping: a leaf that "restates the posture" must restate the
ENTIRE arm-32 leaf (anchor + rx datum + sbasis_complete), not just the two
mask knobs the §1.5 note names.**

### F7. 20-ep smoke verdicts (corrected posture) — ALL PASS

All three: 20/20 epochs emitted, 0 withheld, 34 s wall each.

**Card S2H (`runs/s2h_20ep_on`)**: transform applied 20/20 epochs, family
GPS_DCB, n = 61 stations every epoch, 0 thin-skips, 0 screened votes.  μ per
epoch: −0.38 m at the first harvest, then a smooth +0.02…+0.47 m band with NO
epoch-to-epoch alternation (P3 ✓ — no loop to ring), declining toward +0.02 by
epoch 20; Σλ grows 27 → 1306 as posteriors tighten.  Reading: during ignition
the per-epoch solve re-accumulates ~0.3 m of common mode per epoch — exactly
the content the rolling carry used to transport (the flywheel), now removed at
every harvest.  Control run: knob lines absent (P1 ✓ — the two grep hits were
the config FILENAME).

**H-rider (b) (`runs/hsig_20ep_on`)**: S line carries `hsig=pc`; H σ̂₀ VARYING
and alive (2.076 → 3.323 over the smoke; run-end line 20/20 usable) — vs the
control's dead constant `0.0000`.  **P2 (the sharp gate): the E/C/B/P record
stream is 0-diff vs control over all 2700 record lines** — the census feed is
solve-neutral by measurement, not just by design.

**H-rider (a) predictions all confirmed** (F6 + this): trace byte-identical,
121/121 heartbeats forwarded, first `producer status: (none) -> ok` line in
batch-replay history.

### F8. Card S2H 1 h stability gates — ALL FIVE PASS (batch B)

Arms: `runs/s2h_3h_ctl` (arm-32 posture, session binary, 360/363 emitted, 232 s)
and `runs/s2h_1h_on` (one knob, 121/123 emitted, 89 s).  Settled window =
20–60 min, time-matched.

| gate | threshold | ON arm | control | verdict |
|---|---|---|---|---|
| G1 a₀₀ settled sd | ≤ 5 TECU, no alternation | **2.92**, flip-frac 0.37 | 2.51, 0.35 | **PASS** |
| G2 fold fraction (hour-1, matched 7313 rows) | ≥ control | **0.425** | 0.356 | **PASS +19 %** |
| G3 prires rxcb σ̂₀ (geo_t2 med, settled) | ≤ 1.5× control | **4.43 = 0.90×** | 4.91 | **PASS — QUIETER** |
| G4 rms_phase (STA median, settled) | within ±5 % | **8.20 mm (+2.5 %)** | 8.00 mm | **PASS** (LV-4 paid +32 %) |
| G5 s2h counters | applied, smooth μ | **121/121, 0 thin, 0 screened; μ → +0.0000 m (mean 0.047)** | knob-off silent | **PASS** |

Two findings worth the card's headline:
1. **The fold-side cost is ~zero AND the prior side is QUIETER** — with the
   data side flat (+2.5 %) this is NOT the lane over-retention trade (that
   signature is quiet priors bought with 2.3× louder data).  De-meaned carried
   targets sit closer to the solve, so they both fold more often (G2) and fit
   better when they fold (G3).
2. **μ converges to zero**: ignition removes ~0.3 m/epoch of re-accumulating
   common mode; settled, the carried set is zero-mean and STAYS zero-mean
   (μ last +0.0000, mean |μ| 0.047 m over the hour) — the transform's fixed
   point, i.e. nothing left for a rolling memory to transport.
Level (diagnostic, rank-only): ON settles lower (mean 19.5 vs 24.2 TECU,
20–60 min) — direction consistent with the per-epoch data+datum equilibrium
prediction; the legal read comes from the 3 h arm (running).

### F9. Card S2H 3 h gates — ALL PASS; the zero-fold-side-cost claim is VALIDATED

Arms: `runs/s2h_3h_on` (360/363 emitted, 227 s) vs `runs/s2h_3h_ctl`.
The control **reproduces stored arm 32's hourly GIM diffs EXACTLY**
(−7.23/−5.52/+0.44) — rebuild determinism holds at the diagnostic level too.

| 3 h gate | ON | control | verdict |
|---|---|---|---|
| G1 h2-3 rms_phase (±5 %) | **9.60 mm (+2.1 %)** | 9.40 mm | **PASS** — flat with era (settled +2.5 %), not compounding; LV-4 paid +32 % here |
| G2 prires rxcb σ̂₀ settled → h2-3 (≤1.5×) | **4.43 → 4.95 (0.90×/0.89×)** | 4.91 → 5.55 | **PASS — quieter at BOTH windows, growth ratio same as control** (LV-4 doubled: 3.10 → 6.33) |
| G3 folds h2-3 / sh_health | **0.523**, 360/360 emitted | 0.487 | **PASS** |
| G5 wire churn (step census) | 60 key-steps / 33 ep, clock 0/0 | 59 / 34 ep, clock 0/0 | **PASS — statistically identical; the removed common mode is datum-null, invisible on the wire** |

G4 level, DIAGNOSTIC (invariant #5; time-matched GIM + footprint):
ON −13.58 → −11.36 → **−4.35 ± 1.75** hourly vs control −7.23 → −5.52 → +0.44;
footprint-restricted mean −13.26 (ON) vs −2.76 (ctl).  Reading: with the
carry's common-mode memory zeroed each epoch, the level sits at the per-epoch
data+datum equilibrium — LOWER, toward the zero-mean-convention frame, still
converging upward at h2-3, and with NO carry-off-class collapse (that mode was
−9 TECU/EPOCH; this is a stable ±1.75 band).  The DEFECT-2 convention offset
(fleet −5 ns DCB mean) is now a DECLARED frame property drained per-epoch —
whether the a₀₀ anchor's authority (no longer outvoted by the carry, but still
outvoted by per-epoch data GF information) suffices to land the 12 h target is
exactly the DEFERRED stage-3 question, and the §13.4-option-1 / item-0b
(external receiver DCB) follow-up is its designed answer if it does not.

**Standing: card S2H is BUILT, oracle-validated, OFF-byte-identical, and
field-validated through 3 h on every fold-side gate it was designed to win.
Not armed by default (no-promotion rule); 12 h tracking verdict deferred.**

### F10. Edge 2 — clock_datum_sum_to_zero's FIRST validation run (wiring smoke, leveled lineage)

Pair `runs/evt_pl_clkpin` (leveled pivot-pin baseline: inherited EXT-CB clock
HOLD killed in both arms; sticky pivot goes live) vs `runs/evt_pl_clks2z`
(one knob: `clock_datum_sum_to_zero`), session binary, both 121/121 emitted,
0 withheld.  Every predicted loader interlock fired: PIN arm shows sticky
clock-pivot LIVE (no INERT warn); S2Z arm shows the SUM-TO-ZERO banner,
sticky_pivot INERT warn, and the datum-graph declared-d=0 semantics warn.
Clock-channel census: **CLEAN on both** (0 common-mode steps > 0.1 m, 0
per-sat outliers > 0.5 m over the hour); 0 pivot/handover/set-change events in
either arm (quiet hour, sticky pivot never handed over — so the event classes
the 12 h A/B will count simply did not occur in this window, as the power
prediction said).  Both arms carry 2 R records (the leveled mark chain
reproduces).  **Standing change: s2z upgrades from "never validated — do not
arm blind" to "wiring-validated on the leveled lineage"; the 13-jumps/12 h
jump-count A/B (the actual Edge-2 verdict) needs the 12 h pair and stays
deferred.**  Configs `_ext63_1h_evt_pl_clk{pin,s2z}.json` carry the
predictions; a 12 h continuation extends them directly.

### F11. Concurrent-workstream verification (for the compound commit)

- sim workstream (`src/sim`, `include/sim`): `gnss_simulator_selftest` builds
  and passes **35/35** against the uncommitted iono-disturbance changes.
- pyviz workstream: **326 passed / 3 skipped** under the repo venv
  (`venv\Scripts\python.exe`; system 3.13/3.14 lack pandas — run the suite with
  the venv, not bare `python`).
- Incidental process lesson re-paid tonight (memory `feedback_wer_zombie_wedge`
  + a new one): a PowerShell pipeline with `Select-Object -First N` KILLS the
  upstream producer when N matches arrive — never grep a live producer's
  stdout with early-terminating pipelines; redirect to a file and grep after.
  The killed producer left a zombie holding the shared spdlog dir (0xC0000409
  on the next run) — pre-kill name-scoped before rerunning.
