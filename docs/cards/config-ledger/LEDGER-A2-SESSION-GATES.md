# A2 — the F-series session gates — S2H, the H-heartbeat rider, the internal 12 h stability verdict, and the promotion shortlist

**STATUS**

- **id:** A2
- **topic:** config-ledger
- **status:** closed
- **summary:** the F-series session gates — S2H, the H-heartbeat rider, the internal 12 h stability verdict, and the promotion shortlist
- **dates in the text:** 2026-08-08 → 2026-08-12
- **knobs / posture:** `network.rx_cb_harvest_demean`, `network.rx_cb_harvest_screen_z`, `network.h_sigma0_prefit_census`, `network.h_sigma0_classes`, `estimation.global_vtec_sh`, `network.rx_code_bias_datum`, `network.sbasis_complete`, `network.clock_datum_sum_to_zero`, `network.mark_refsat_reset_disc`, `network.emit_pb_filter_gate_v2` … (+10 more in the text)
- **hashes named in the text** (commit / exe / byte-cert): —
- **run dirs named in the text:** `runs/ext63_1h_evt_pl`, `runs/s2h_20ep_on`, `runs/hsig_20ep_on`, `runs/s2h_1h_on`, `runs/s2h_3h_ctl`, `runs/s2h_3h_on`, `runs/s2h_12h_`, `runs/golden63_12h_internal`
- **source:** `docs/archive/CONFIG_promotion_ledger.md` — A2

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: CONFIG_promotion_ledger.md -->
## A2 F0 / F4 — suite baselines

**F0:** Debug `gnss_tests` against the uncommitted tree at session start — **1274 passed / 1 skipped
(fixture-gated) / 0 failed** (1275 cases, up from 1264 at the Aug-5 hand-back). **F4:** with both new
cards in — **1298 passed / 1 skipped (1299 cases, +24)**, including the LASER GPS3710 PMINOLESS
golden-oracle test (the port recovers LASER's published PMINOLESS realization from a re-pinned RLESS
input on the real golden CSV, per-point ≤ 5e-5 m, and the SQSᵀ covariance closed form matches an Eigen
brute force to 1e-12). Both counts are the session's own denominator; the current suite size is
CLAUDE.md's 1360 cases / 196 suites / 89 files (re-counted 2026-08-12 under VS 2026).

## A2 F1 — orientation findings from existing artifacts (free)

1. **Within-epoch RAIM on the leveled arm is ALIVE.** `runs/ext63_1h_evt_pl`
   `network_flag_rates.csv` tier **T2: 115 opportunities / 0 flags** — live on exactly the 115
   `carry_warm` epochs (the gate opened at epoch 7 per `phase_leveling_warmup_epochs: 6`); quiet hour,
   so no trigger. T3 / T3c / T4: 0 opportunities.
2. **H σ₀ on the leveled arm is DEGENERATE, not dead.** 121 H records: **6 × `0.0000`** (warm-up) then
   **115 × a constant `1.0000` with `n_adapt = 0` every epoch** — the vacuous-pool publication, not a
   live σ̂₀. Leveling un-zeroes the field; the wire still carries no usable statistic. This motivated
   the census feed (F3).
3. **The Q4 non-detection was UNPOWERED BY GEOMETRY.** G16 @ TOW 259470 is visible from the Pacific
   (MAJU/KOKV/MIZU), G11 @ 262020 from the South Atlantic (STHL/WIND/PALM) — **intersection EMPTY**,
   and the held-out consumer WTZZ00DEU sees neither. Slip origins: G16 ← KOKV00USA, G11 ← STHL00GBR
   (both also near-highest-elevation observers).
4. ⚠ **`_ext63_1h_evt_pl.json` lacks `state_snapshot_require`**, so a fingerprint mismatch would
   SILENTLY cold-start on a resumed leg (invariant #4). Noted for any future warm-resume use; that
   night's arms were all fresh-convergence.

## A2 F2 — Card S2H: `network.rx_cb_harvest_demean` (knob contract)

**Knobs:** `network.rx_cb_harvest_demean` + `network.rx_cb_harvest_screen_z` (**default 8.0**);
**default OFF, FINGERPRINTED (`rxcbharv:`)**; forces the superseded LV-2 / LV-5 fold-side de-mean OFF
with a warn. **Mechanism:** per family (CB state key), at the serial post-solve harvest, AFTER the
LV-4 erase pass and the gap prune (member set = exactly the fold-eligible set), subtract the
**Λ-weighted mean (λ = 1/var)** of the harvested **ABSOLUTE** realizations from every carried entry,
with the exact diagonal covariance co-transform (voters var − 1/Σλ, screened var + 1/Σλ =
diag(S·Q·Sᵀ)). Robust peer screen on the μ vote (median/MAD, floor = median formal σ, z = 8) **per
SPEC §16/12.1**. **Thin epochs apply NOTHING (identity, counted — never a frozen μ.)** Trace: one
`s2h_mu` events row per family per epoch; armed-only run-end line. **Two load-bearing deviations from
the LASER original:** (1) de-mean about the **DATUM ROW's** absolute-zero target, **NOT about x_lin** —
de-meaning about x_lin is LV-2's measured failure (it pins mean(x_lin)); (2) **Λ-weighted**, because
the fold's common-mode force is ∝ λ, so only the weighted mean zeroes the NET force (λ ≡ const reduces
exactly to the LASER oracle case). **Touchpoints:** `NetworkRunTypes.hpp` (`rxCbHarvestMu` pure
helper), `NetworkRunContext.{hpp,cpp}` (knobs, carry struct, fingerprint, harvest pass),
`GnssConfig.hpp`, `GnssConfigLoadNetwork.cpp` (parse + guards), `NetworkLeveling.cpp` (trace),
`NetworkTeardown.cpp` (run-end line), CMakeLists. **Tests: `tests/test_rx_cb_harvest_demean.cpp` — 14
cases** (Λ-mean identities, idempotence, realization invariance, the **LASER GPS3710 PMINOLESS golden
oracle** skip-gated on `C:\LASER`, per-point ≤ 5e-5 m; **SQSᵀ closed form vs Eigen brute force to
1e-12**; peer screen; thin/degenerate; loader contract).

## A2 F3 — the H-heartbeat rider: `network.h_sigma0_prefit_census` (knob contract)

The H-rider pair is **§27 item 2c / edge-queue §1.6 item 3**. **(a) Batch replay reads H.**
`NetworkProductReader::read` gained an **optional 4th out-param** collecting EVERY H in file order,
**including standalone WITHHELD-epoch H's** (which complete no record and were invisible to both the
records and `epoch_status`). `ApplicationConsumerReplay` forwards them to
`SsrCorrectionStore::setProducerStatus` interleaved by content epoch, deduped — the batch analogue of
the RTA-6 tail wiring in ApplicationRealtime — with an invariant-#4 run-end count ("N on the wire, M
forwarded"). Solve determinism untouched (the status channel is observability; `logProducerStatus`
logs one-shot transitions). **(b) Census-fed H σ₀.** Knobs `network.h_sigma0_prefit_census` (**default
OFF; REQUIRES `emit_epoch_status_record` + `trace_prefit_census`, INERT-warn otherwise**) and
`network.h_sigma0_classes` (**default `"code,sh,other"`** — phase excluded per census deviation 2c).
`networkPrefitSigma0` in **`NetworkPrefitCensus.cpp`** uses the same statistic / centres / taxonomy as
INC-1 (common-mode-removed s = |y−m|·√w), pooled across stations over the class mask, **σ̂₀ = 1.4826 ·
median**; born-cell deferral via the census's lag-1 presence map; pin-regime rows excluded; **empty
pool → 0.0 = the H grammar's "unavailable" sentinel**. Computed **ONCE per epoch PRE-solve at the top
of `networkSolveGlue`** so **ALL FOUR H sites** carry it — including the pre-solve gate withhold, whose
σ₀ was structurally 0. UNIFORM statistic per wire; advertises `hsig=pc` on the S contract
(**byte-identical when absent**, legend updated); the A4 withhold ceiling still reads `sigma0_adapt`,
unchanged. **Tests: `tests/test_h_heartbeat_rider.cpp` — 7 cases** (batch-read surfacing incl. the
standalone withheld H, 3-arg source-compat, hsig round-trip + byte-absence, loader contract).

## A2 F6 — Q4 CLOSED: the R-record consumer link, proven end-to-end

*Power fix:* two single-event replay pairs with **non-origin high-elevation** observers as consumers
(the F1(3) geometry finding), against byte-exact marks/no-marks product twins.

| pair | mark | marks arm | nomarks arm |
|---|---|---|---|
| MAJU (MAJU00MHL, G16 el 62°) | `R,G16` @ TOW 259470 | **reset G16 L1C+L2W @ exactly 259470** | none at 259470 |
| WIND (WIND00NAM, G11 el 48°) | `R,G11` @ TOW 262020 | **reset G11 L1C+L2W @ exactly 262020** | none at 262020 |

Shared in both twins: the 259230 ignition resets and WIND's 262200 value-step reset — the mark resets
are ADDITIONAL and mark-caused; the two products are byte-exact apart from the R rows. **Chain
validated: IF-TD `refsat_reset` (carry_warm) → mark → `R` record → `markSatDiscontinuity` → both-band
consumer ambiguity reset at the event epoch.** Caveats: consumers are IN-NETWORK (mechanism proof,
**not** an accuracy grade), and the net VALUE of marking is **unquantified** — WIND marks hour-0 3D max
**38.9 m vs 12.8 nomarks** is the reset transient's face.
**F6 config-chain lesson, re-paid (three 20-ep smokes returned 0 epochs before it was found):** a leaf
config that "restates the posture" must restate the **ENTIRE arm-32 leaf** —
`estimation.global_vtec_sh` (the a00 anchor 32.24 ± 2), `network.rx_code_bias_datum` and
`network.sbasis_complete` are **NOT in the base chain**. Without anchor + datum the level and rx-DCB
directions are genuinely open and the observability gate correctly withholds EVERY epoch (identical
inputs, `gate_ok` 0 vs 1).

## A2 F7 — 20-ep smoke verdicts (corrected posture) — ALL PASS

All three arms 20/20 epochs emitted, 0 withheld, 34 s wall each.
**S2H (`runs/s2h_20ep_on`):** transform applied 20/20, family GPS_DCB, n = 61 stations every epoch, 0
thin-skips, 0 screened votes; μ −0.38 m at the first harvest then a smooth +0.02…+0.47 m band with
**NO epoch-to-epoch alternation**, declining toward +0.02 by epoch 20; Σλ grows 27 → 1306 as posteriors
tighten. Reading: during ignition the solve re-accumulates ~0.3 m of common mode per epoch — the
content the rolling carry used to transport (the flywheel), now removed at every harvest. Control run:
knob lines absent.
**H-rider (`runs/hsig_20ep_on`):** S line carries `hsig=pc`; **H σ̂₀ varying and alive 2.076 → 3.323**
(run-end 20/20 usable) vs the control's dead constant `0.0000`; **the E/C/B/P record stream is 0-diff
vs control over all 2700 record lines** — the census feed is solve-neutral **by measurement**, not only
by design. 121/121 heartbeats forwarded; first `producer status: (none) -> ok` line in batch-replay
history.

## A2 F8 — Card S2H, 1 h stability gates — ALL FIVE PASS

Arms `runs/s2h_1h_on` (one knob, 121/123 emitted, 89 s) vs `runs/s2h_3h_ctl` (arm-32 posture, session
binary, 360/363 emitted, 232 s). Settled window 20-60 min, time-matched.

| gate | threshold | ON arm | control | verdict |
|---|---|---|---|---|
| G1 a₀₀ settled sd | ≤ 5 TECU, no alternation | **2.92**, flip-frac 0.37 | 2.51, 0.35 | **PASS** |
| G2 fold fraction (hour-1, matched 7313 rows) | ≥ control | **0.425** | 0.356 | **PASS +19 %** |
| G3 prires rxcb σ̂₀ (geo_t2 med, settled) | ≤ 1.5× control | **4.43 = 0.90×** | 4.91 | **PASS — QUIETER** |
| G4 rms_phase (STA median, settled) | within ±5 % | **8.20 mm (+2.5 %)** | 8.00 mm | **PASS** (LV-4 paid +32 %) |
| G5 s2h counters | applied, smooth μ | **121/121, 0 thin, 0 screened; μ → +0.0000 m (mean 0.047)** | knob-off silent | **PASS** |

**Headline: the fold-side cost is ~zero AND the prior side is QUIETER with the data side flat — this is
NOT the lane over-retention signature** (that one is quiet priors bought with 2.3× louder data).
De-meaned carried targets sit closer to the solve, so they fold more often (G2) and fit better when
they fold (G3). μ converges to zero and STAYS zero-mean settled (mean |μ| 0.047 m over the hour) — the
transform's fixed point, i.e. nothing left for a rolling memory to transport. Level (diagnostic,
rank-only): ON settles lower, mean 19.5 vs 24.2 TECU over 20-60 min — ⚠ **the legal read comes from the
3 h arm (F9), not from this one.**

## A2 F9 — Card S2H, 3 h gates — ALL PASS

`runs/s2h_3h_on` (360/363 emitted, 227 s) vs `runs/s2h_3h_ctl`. The control **reproduces stored
arm 32's hourly GIM diffs EXACTLY** (−7.23 / −5.52 / +0.44) — rebuild determinism holds at the diagnostic
level too.

| 3 h gate | ON | control | verdict |
|---|---|---|---|
| G1 h2-3 rms_phase (±5 %) | **9.60 mm (+2.1 %)** | 9.40 mm | **PASS** — flat with era, not compounding; LV-4 paid +32 % here |
| G2 prires rxcb σ̂₀ settled → h2-3 (≤1.5×) | **4.43 → 4.95 (0.90× / 0.89×)** | 4.91 → 5.55 | **PASS — quieter at BOTH windows, growth ratio = control's** (LV-4 DOUBLED 3.10 → 6.33) |
| G3 folds h2-3 / sh_health | **0.523**, 360/360 emitted | 0.487 | **PASS** |
| G5 wire churn (step census) | 60 key-steps / 33 ep, clock 0/0 | 59 / 34 ep, clock 0/0 | **PASS — statistically identical; the removed common mode is datum-null, invisible on the wire** |

G4 level, DIAGNOSTIC (invariant #5, time-matched GIM + footprint): ON −13.58 → −11.36 → **−4.35 ±
1.75** hourly vs control −7.23 → −5.52 → +0.44; footprint-restricted mean −13.26 (ON) vs −2.76 (ctl).
With the carry's common-mode memory zeroed each epoch the level sits at the per-epoch data+datum
equilibrium — LOWER, toward the zero-mean-convention frame, still converging upward at h2-3, and with
**NO carry-off-class collapse** (that mode was −9 TECU/EPOCH; this is a stable ±1.75 band). DEFECT-2's
convention offset (fleet −5 ns DCB mean) is now a DECLARED frame property drained per epoch.
**Standing after 3 h: card S2H is BUILT, oracle-validated, OFF-byte-identical and field-validated
through 3 h on every fold-side gate it was designed to win. Not armed by default (no-promotion rule).**

## A2 F12 — Card S2H stage 3 (12 h): fold-side PASSES stronger; the level target is NOT met by EITHER arm, and the wander is proven NOT-the-carry

Arms `runs/s2h_12h_{ctl,on}`, **1440/1440 epochs each**, ~14.5 min wall, same binary; predictions
pre-registered in the configs.

| gate | ON | control |
|---|---|---|
| rms_phase med h2-6 / h6-12 | 12.70 / **16.00 mm (+0.8 % / +0.6 %)** | 12.60 / 15.90 |
| prires rxcb med h2-6 / h6-12 | 8.43 (1.03×) / **10.91 (0.92× — quieter late)** | 8.17 / 11.91 |
| prires era-growth h2-6 → h6-12 | **1.29×** | 1.46× |
| fold fraction h2-6 / h6-12 | **0.608 / 0.596** | 0.570 / 0.542 |
| wire: PB key-steps / epochs, clock | 197 / 131, 0/0 | 196 / 130, 0/0 |
| S2H counters | 1440/1440 applied, 0 thin, **693 screened votes (~0.8 %)**, μ last +0.0000, mean \|μ\| 0.0040 m | — |

**P3 — the tracking verdict: the ±4-at-h0-AND-h11 target is NOT reached by EITHER posture.**
Time-matched hourly a₀₀−GIM: control mean **−9.60 sd 6.09** (reproducing the canonical −4…−15 band
exactly), ON mean **−12.21 sd 5.41** (~2.6 lower, spread SMALLER). Both arms **dive together h4-h7**
(ctl −7.4 → −18.6; ON −12.5 → −19.9) and recover together. **⇒ THE CARRY'S ROLLING MEMORY IS EXCLUDED
as the wander's cause** — S2H provably zeroes that memory every epoch (μ ≈ 0, 1440/1440) and the wander
persists at nearly full amplitude. What both postures share: (a) the per-epoch admitted GF data
equilibrium (the fleet DCB convention), and (b) the **FIXED a₀₀ anchor at 32.24 against a GIM global
mean running a 32.0 → 36.9 diurnal**, which alone contributes **−4.6 TECU by h6** even for a perfectly
anchor-tracking field. Consequence: the a₀₀ anchor's authority is insufficient on this ladder
**independent of the carry vehicle**; the designed next authority is external RECEIVER DCBs (CAS
MGEX) — the **§13.4-option-1 / item-0b** follow-up is its designed answer — whose only blocker was the
parser card in F14 (a GIM-valued or diurnal anchor stays ladder-diagnostic-only under the
no-external-data-dependence directive). S2H's own standing is unchanged and strengthened:
correct-by-construction frame drain at zero measured cost, now through 12 h.

## A2 F10 + F13 — Edge 2, `clock_datum_sum_to_zero`: wiring smoke then the 12 h jump-count A/B

**Recorded in full in the `network.clock_datum_sum_to_zero` Tier-2 entry above** — the fired loader
interlocks, both clock censuses, the −15 %-resets / higher-median two-sided consumer trade, and the ⚠
that the 13-jumps/12 h figure belongs to the FT63/golden lineage rather than this one. **Verdict:
wiring-validated and excursion-suppressing but it does NOT graduate as strictly better — the pin datum
stays the default**, both signatures measured and on record.

## A2 F14 — the ProductParser receiver-bias fix (unblocks item 0b, external receiver DCBs)

**Defect:** the BIAS/SOLUTION loop **silently skipped every station-bearing record** —
`ProductParser.cpp` old **:1118** `if (!station.empty()) continue;` — dropping **6797 records/day from
the COD MGX file alone** — and rejected **DSB** records outright, which is exactly the record type the
**CAS MGEX DCB** product uses to carry NONZERO receiver DCBs. **Built:** `ReceiverBiasData`
(ProductTypes) + **`ProductParser::getReceiverBiases()`**; the loop now parses receiver **OSB** lines
(COD format: 1-char system, 4-char station) and receiver **DSB** lines (CAS format, both signals) into
a **SEPARATE receiver vector**, with the satellite vector structurally untouched (unit-tested identical
with/without receiver content). Satellite DSB lines are counted skips (out of scope until something
needs them). An all-receiver file (the CAS DCB shape) is now a legal parse; empty-both still fails
loud. **NOTHING consumes the receiver vector yet** — the estimation rung (pinning rx-DCB states) is its
own default-OFF card, gated on the CAS product being on disk. **Tests:
`tests/test_bia_receiver_records.cpp` — 7 cases** incl. the real COD file (fixture-gated), asserting
the **6797** count and COD's GPS-receiver-zeros convention.

## A2 F15 — THE INTERNAL (no-external) 12 h stability verdict

Arm `runs/golden63_12h_internal` (`_golden63_gps_12h_internal.json`): the self-contained golden63
posture — own satellite clocks + phase biases, SP3 orbits only, live WTZZ00DEU closed-loop consumer,
full golden instrument set plus the session's solve-neutral observability (contract, heartbeat, census
σ̂₀). **1440/1440 emitted, 0 withheld, 0 SH-gate eras, 31 min wall.**

**STABLE in the divergence sense — a real milestone for this lineage:** the consumer is BOUNDED all day
(hourly medians 3.1–15.4 m, worst single epoch **20.7 m**; the 300-1000 m excursion class of the Aug-5
12 h replays is GONE under the current armed set); a₀₀ stays in the physical band (ends **40.8 TECU**,
W6-convention wander only) with no level collapse, no withhold eras and no gate flapping; the H wire
carries a live varying σ̂₀ on all 1440 epochs (`hsig=pc`) — the internal lineage's first usable
per-epoch health statistic.

**NOT stable in the product-consistency sense — now precisely quantified:** the emitted **CLOCK
re-realizes on 613 of 1440 epochs** (census common-mode steps > 0.1 m; **273 epochs with per-sat
outliers > 0.5 m**) against **0/0** on the external-clock lineage measured the same morning.
**CLAUDE.md amendment 5's "13 product-wide jumps / 12 h" was the tip: at the 0.1 m census floor the
re-realization is ENDEMIC, not episodic.** PB churn **12.9 % of epochs** (292 key-steps, |step| up to
129 m) vs ~9 % on the EXT lineage. All of it rides on **ONE `datum_changed` mark** — the wire is
event-blind to the churn class BY DESIGN (the clock channel has no magnitude jump test; the churn is
re-realization, not datum handover). Consumer medians drift upward through the day (worst hour LAST);
**AR fixes 0/1440**, the AGU26 churn-blocks-fixing verdict intact with the churn now instrumented
end-to-end. **Answer to "is it finally stable": the divergence/excursion instability is fixed; the
remaining instability is product-internal consistency — the network-estimated clock's continuous
re-realization — and it is now measured, bounded, wire-visible (census), and the correct next card.**
That card was written: Card CLK-R, `docs/SPEC_clock_emit_rebase.md`, whose `emit_clock_frame_hold`
takes the internal census **613 → 1** (see its Tier-1 entry above); FC-1's churn ceiling of 2.3 % was
the design input.

## A2 F16 — the golden-config promotion shortlist, and what was APPLIED

**✅ APPLIED 2026-08-08 on explicit user sign-off ("apply all the confirmed enhancements")** into
`_golden63_gps_1h_laneoff.json` (+ the laneon twin, keeping the lane A/B one-knob-different) and
`_golden305_gps.json`; every descendant including the whole `_ext63` family inherits (verified via
chain flatten + posture check). The historical `_golden0729_*` arms are **stored denominators and were
NOT touched.** ⚠ **The md5-pairing boundary this creates is BY DESIGN:** promoted arms gain the S
`hsig=` token and 1 H line per epoch, so **whole-file md5 pairing across that boundary BREAKS**; the
E/C/B/P record streams stay comparable.

| knob | evidence | class | recommendation | outcome |
|---|---|---|---|---|
| `serialize_sbasis_contract` | replay consumer **−12.7 % RMS, 45 spurious resets removed**; header-only, solve-byte-identical | fingerprint-SAFE | **PROMOTE into the golden/base configs** | **APPLIED** |
| `emit_epoch_status_record` + `h_sigma0_prefit_census` | record stream **0-diff** (solve-neutral measured); live σ̂₀ on EXT and INTERNAL wires; the H grammar's 0-sentinel preserved | wire-only | **PROMOTE (pair them)** | **APPLIED** |
| `network.mark_refsat_reset_disc` | producer chain + consumer link PROVEN (F6); net value unquantified (reset-transient trade); inert without `phase_leveling` | leveled lineages only | promote to LEVELED configs with the caveat | NOT applied |
| `rx_cb_harvest_demean` (S2H) | zero fold-side cost through 12 h on EXT-CB; solve-affecting | fingerprinted | keep default-OFF; arm freely on EXT diagnostic arms; **golden lineage needs its own A/B first** | NOT applied |
| `clock_datum_sum_to_zero` | 12 h A/B: excursion-suppressing but higher steady median — **does not graduate** | fingerprinted | **do NOT promote; pin stays default** | NOT applied |
| root `elevation_cutoff_deg` 15→25 | mask study §26 (prior session) | posture | still awaiting the user's Aug-5 sign-off | NOT applied |

## A2 F11 — concurrent-workstream verification, and one process lesson

`gnss_simulator_selftest` **35/35** against the uncommitted sim iono-disturbance changes; pyviz **326
passed / 3 skipped** under the repo venv (`venv\Scripts\python.exe` — system 3.13/3.14 lack pandas, so
run the suite with the venv, not bare `python`). ⚠ **Process lesson, re-paid: a PowerShell pipeline
with `Select-Object -First N` KILLS the upstream producer when N matches arrive.** Never grep a live
producer's stdout with an early-terminating pipeline — redirect to a file and grep after. The killed
producer left a **zombie holding the shared spdlog dir (0xC0000409 on the next run)**; pre-kill
**NAME-SCOPED** before rerunning. Pairs with the standing concurrent-build discipline: run a renamed
copy of the exe so the linker never hits a locked binary — and re-copy + hash it, because a renamed exe
is a SNAPSHOT of the build at copy time.

## A2 — session gate results that certify its own binaries

- **OFF-path structural byte-identity across the rebuild: PASS.** `ext63_1h_evt_off_s2hbin` vs the
  stored anchor — **16,286 of 16,287 lines byte-identical** (every E/C/B/P/H/S record); the single diff
  is the `#` legend comment line now documenting the optional `hsig=` token. 121/123 epochs emitted, 0
  gate-withheld, run 91 s.
- **Replay solve-identity with H-forwarding live: PASS byte-identical.** `replay_1h_pl_marks_s2hbin` —
  `closed_loop_trace.csv` md5 **`E12AE34433BC…`** equals the stored original exactly; the status channel
  is pure observability, solve untouched.

---

# Appendix A3 — Campaign-anchor → knob index

Which appendix anchor supplies the evidence quoted in which Tier entry, so a future auditor can
re-point a claim without re-reading the campaign. Every row's tier claim was checked programmatically
against the `###` list; the last row is the exception and says so.

| Tier entry | anchor | what it supplies |
|---|---|---|
| `network.emit_pb_filter_gate_v2` (Tier 1) | **§A1 R1 (G3)** and **§A1 R3 (G4/G5)** | the raw A/B tables behind "gates G1-G5 ALL PASS", the v1 death-spiral numbers, "solve BYTE-IDENTICAL ⇒ outputs-only / S-transparent", and the k=4 cross-cadence transfer |
| `network.emit_pb_filter_jump_k / _meas_sigma_m` (Tier 2) | **§A1 R3, the G5 staged edit** | the config-level retirement of the per-cadence v1 hand tuning |
| `network.parallel_carry` (Tier 2) | **§A1 R2** | the 2.13× @20 threads, the identical-counter equivalence set, and the per-thread-count reproducibility rule |
| `network.parallel_max_threads` (Tier 2) | **§A1 R2** | why the thread count must be recorded with any pcarry result |
| the Step-0c mutex cited in `network.parallel_carry`'s interlock | **§A1 Step 0c** | the carry-wins → reduce-wins flip and the three files it touched |
| `network.epoch_decimation_n` (Tier 2) + `network.carry_invariant_innov_k` (Tier 1) | **§A1 R4** | the D5 cross-tier rates, the σ-incest reading, and the k-raise remedy that was later FALSIFIED |
| `estimation.global_vtec_sh.max_degree` + `estimation.ionosphere_weighting.elevation_cutoff_deg` (Tier 2) | **§A1 R5 / R5b** | the em25d15-vs-em10d8 decider table and the 6 h churn ladder |
| (no knob — M5 producer-AR scope) | **§A1 R6** | the third-state verdict; M5's prerequisite is DD convergence, not emission continuity |
| `network.clock_datum_sum_to_zero` (Tier 2) | **§A2 F10 / F13** | the wiring smoke and the 12 h two-sided consumer trade behind "does not graduate" |
| `network.emit_clock_frame_hold` (Tier 1) + `network.clock_disc_jump_test` (Tier 4) | **§A2 F15** | the 613/1440 internal-clock re-realization census Card CLK-R was written against |
| `network.raim_within_epoch` (Tier 2) and the RAIM cross-cutting note | **§A2 F1** | the leveled arm's T2 115-opportunities / 0-flags reading and the degenerate constant-1.0000 H σ₀ |
| ⚠ **NO TIER ENTRY EXISTS** for `network.h_sigma0_prefit_census`, `h_sigma0_classes`, `emit_epoch_status_record`, `serialize_sbasis_contract`, `rx_cb_harvest_demean`, `rx_cb_harvest_screen_z`, `network.mark_refsat_reset_disc` | **§A2 F2 / F3 / F16** | knob contracts, defaults, fingerprint status, and the applied / not-applied promotion dispositions — this row points at APPENDIX PROSE, not at a `###` entry, because none of these seven has one (nor a JSON object; see the membership-drift note in the header). Two of them have SHIPPED into the golden lineage. |

